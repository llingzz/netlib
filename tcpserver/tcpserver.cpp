// tcpserver.cpp — implementation
//
// Wire protocol: 8-byte zero-padded ASCII header + payload body.

#include "tcpserver.h"

#include <charconv>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <iostream>

#include <spdlog/spdlog.h>

// ===========================================================================
// session — member implementations
// ===========================================================================

session::session(std::shared_ptr<tcp::socket> socket, LONG token,
                 const std::function<void(LONG)>& cbRemove,
                 const std::function<void(std::vector<std::string>&&)>& cbData)
    : socket_(std::move(socket))
    , tokenid_(token)
    , cb_remove_(cbRemove)
    , cb_on_data_(cbData)
{
}

void session::start()
{
    do_read();
}

void session::close()
{
    // Idempotent guard — exchange returns the previous value atomically.
    if (closed_.exchange(true)) {
        return;
    }
    asio::error_code ignored;
    socket_->shutdown(tcp::socket::shutdown_both, ignored);
    socket_->close(ignored);
    if (cb_remove_) {
        cb_remove_(tokenid_);
    }
}

void session::post_write(std::string_view data)
{
    if (data.size() > max_payload) {
        return;
    }
    char head[16];
    int hdr_len = snprintf(head, sizeof(head), "%08d", (int)data.size());
    std::string frame;
    frame.reserve(hdr_len + data.size());
    frame.assign(head, hdr_len);
    frame.append(data);
    size_t frame_bytes = hdr_len + data.size();

    bool need_write;
    {
        std::lock_guard<std::mutex> lock(write_mtx_);
        if (write_queue_bytes_ + frame_bytes > write_queue_max_bytes) {
            spdlog::warn("client {} write queue full, dropping message", tokenid_);
            return;
        }
        need_write = write_msgs_.empty();
        write_queue_bytes_ += frame_bytes;
        write_msgs_.push_back(std::move(frame));
    }
    if (need_write) {
        do_write();
    }
}

// --- Read path -------------------------------------------------------------

void session::do_read()
{
    auto self = shared_from_this();
    asio::async_read(*socket_, asio::buffer(read_buf_.data() + data_len_, read_buf_size - data_len_),
        [this, self](asio::error_code ec, std::size_t bytes_read) {
            if (ec) {
                if (ec == asio::error::eof) {
                    if (data_len_ > 0) {
                        parse_messages();
                        data_len_ = 0;       // discard incomplete residual
                    }
                    read_eof_.store(true, std::memory_order_release);
                    try_finish();
                } else {
                    on_error(ec);
                }
                return;
            }
            data_len_ += bytes_read;
            parse_messages();
            do_read();
        });
}

void session::parse_messages()
{
    std::vector<std::string> batch;
    size_t pos = 0;
    while (pos + header_len <= data_len_) {
        int body_len = 0;
        auto fc = std::from_chars(read_buf_.data() + pos, read_buf_.data() + pos + header_len, body_len);
        if (fc.ec != std::errc() || body_len <= 0 || body_len > max_body) {
            spdlog::warn("client {} closed! when parse_messages", tokenid_);
            close();
            return;
        }
        size_t msg_len = header_len + (size_t)body_len;
        // Incomplete — wait for next read.
        if (pos + msg_len > data_len_) {
            break;
        }
        batch.emplace_back(read_buf_.data() + pos + header_len, (size_t)body_len);
        pos += msg_len;
    }
    // Shift residual to buffer front.
    if (pos > 0 && pos < data_len_) {
        memmove(read_buf_.data(), read_buf_.data() + pos, data_len_ - pos);
        data_len_ -= pos;
    } else if (pos == data_len_) {
        data_len_ = 0;
    }
    if (!batch.empty()) {
        on_data_batch(std::move(batch));
    }
}

void session::on_data_batch(std::vector<std::string>&& batch)
{
    request_count_.fetch_add(batch.size(), std::memory_order_relaxed);
    if (cb_on_data_) {
        cb_on_data_(std::move(batch));
    }
}

// --- Write path ------------------------------------------------------------

void session::do_write()
{
    if (write_in_flight_.exchange(true, std::memory_order_acquire)) {
        return;
    }
    auto self = shared_from_this();
    size_t count;
    {
        std::lock_guard<std::mutex> lock(write_mtx_);
        count = write_msgs_.size();
        if (count == 0) {
            write_in_flight_.store(false, std::memory_order_release);
            return;
        }
        if (count == 1) {
            batch_buf_ = std::move(write_msgs_.front());
        } else {
            batch_buf_.clear();
            for (auto& msg : write_msgs_) batch_buf_ += msg;
        }
    }
    asio::async_write(*socket_, asio::buffer(batch_buf_),
        [this, self, count](asio::error_code ec, std::size_t) {
            if (ec) {
                write_in_flight_.store(false, std::memory_order_release);
                on_error(ec);
                return;
            }
            bool more;
            {
                std::lock_guard<std::mutex> lock(write_mtx_);
                for (size_t i = 0; i < count; ++i) {
                    write_queue_bytes_ -= write_msgs_.front().size();
                    write_msgs_.pop_front();
                }
                more = !write_msgs_.empty();
            }
            write_in_flight_.store(false, std::memory_order_release);
            if (more) do_write();
            else try_finish();
        });
}

void session::on_error(asio::error_code ec)
{
    if (ec == asio::error::connection_reset) {
        // RST from client — silent.
    } else if (ec != asio::error::operation_aborted) {
        // server shutdown — silent.
    }
    close();
}

void session::try_finish()
{
    if (!read_eof_.load(std::memory_order_acquire)) return;
    std::lock_guard<std::mutex> lock(write_mtx_);
    if (write_msgs_.empty()) {
        close();
    }
}

// ===========================================================================
// server — member implementations
// ===========================================================================

server::server(std::string ip, int port, int rcvbuf, int sndbuf)
    : m_rcvbuf(rcvbuf), m_sndbuf(sndbuf)
{
    m_token_id = 0;
    m_io_context = std::make_unique<asio::io_service>();

    size_t n_threads = std::thread::hardware_concurrency();
    if (n_threads < 1) n_threads = 1;

    // I/O thread pool — direct pool, no wrapper thread.
    m_work_threads.reserve(n_threads);
    for (size_t i = 0; i < n_threads; ++i) {
        m_work_threads.emplace_back([this] {
            asio::io_service::work work(*m_io_context);
            m_io_context->run();
        });
    }

    // Business thread pool — raw threads, lock-free MPSC queue per thread.
    // Same token → same queue → strict FIFO serialization per token.
    size_t n_biz = std::max(size_t(2), n_threads);
    m_biz_queues.reserve(n_biz);
    for (size_t i = 0; i < n_biz; ++i) {
        m_biz_queues.emplace_back(std::make_unique<MpscQueue<256>>());
    }
    m_biz_threads.reserve(n_biz);
    for (size_t i = 0; i < n_biz; ++i) {
        m_biz_threads.emplace_back([this, i] {
            auto& q = *m_biz_queues[i];
            while (!m_biz_stop.load(std::memory_order_acquire)) {
                std::vector<std::string> batch;
                if (q.try_dequeue(batch)) {
                    do {
                        process_batch(batch);
                    } while (q.try_dequeue(batch));
                } else {
                    std::this_thread::yield();
                }
            }
            // Drain residual after stop.
            std::vector<std::string> batch;
            while (q.try_dequeue(batch)) {
                process_batch(batch);
            }
        });
    }

    // Acceptor setup.
    m_acceptor = std::make_unique<tcp::acceptor>(*m_io_context);
    tcp::resolver resolver(*m_io_context);
    auto endpoints = resolver.resolve(ip, std::to_string(port));
    tcp::endpoint endpoint(*endpoints);
    m_acceptor->open(endpoint.protocol());
    m_acceptor->set_option(tcp::acceptor::reuse_address(true));
    m_acceptor->bind(endpoint);
    m_acceptor->listen();

    // Dynamic accept: post N accepts initially, repost one per completion.
    for (size_t i = 0; i < n_threads; ++i) {
        do_accept();
    }
}

server::~server()
{
    shutdown();
}

void server::shutdown()
{
    if (m_shutdown.exchange(true)) return;

    // Close acceptor to stop new connections.
    if (m_acceptor) {
        asio::error_code ec;
        m_acceptor->close(ec);
    }

    // Copy sessions out under lock, then close without lock
    // to avoid deadlock (close → remove_session → re-acquire lock).
    std::vector<std::shared_ptr<session>> sessions_copy;
    {
        std::unique_lock lock(m_sessions_mtx);
        sessions_copy.reserve(m_sessions.size());
        for (auto& [token, sess] : m_sessions) {
            if (sess) sessions_copy.push_back(sess);
        }
        m_sessions.clear();
    }

    for (auto& sess : sessions_copy) {
        sess->close();
    }

    // Stop business threads first, then I/O.
    m_biz_stop.store(true, std::memory_order_release);
    for (auto& t : m_biz_threads) {
        if (t.joinable()) t.join();
    }

    if (m_io_context) {
        m_io_context->stop();
    }
    for (auto& t : m_work_threads) {
        if (t.joinable()) t.join();
    }
}

void server::send_message(LONG token, const std::string& msg)
{
    std::shared_lock lock(m_sessions_mtx);
    auto it = m_sessions.find(token);
    if (it == m_sessions.end() || !it->second) return;
    it->second->post_write(msg);
}

void server::remove_session(LONG token)
{
    std::unique_lock lock(m_sessions_mtx);
    m_sessions.erase(token);
}

std::string server::deserialize_data(std::string_view data)
{
    return std::string(data);
}

void server::on_session_data(LONG token, std::vector<std::string>&& batch)
{
    auto& q = *m_biz_queues[token % m_biz_queues.size()];
    while (!q.try_enqueue(batch)) {
        std::this_thread::yield();  // queue full — extremely rare
    }
}

void server::process_batch(std::vector<std::string>& batch)
{
    for (auto& data : batch) {
        auto msg = deserialize_data(data);
        // spdlog::info("received from client {}: {}", 0, msg);
    }
}

void server::do_accept()
{
    auto sock = std::make_shared<tcp::socket>(*m_io_context);
    m_acceptor->async_accept(*sock,
        [this, sock](asio::error_code ec) {
            if (!ec) {
                // Set TCP buffer sizes for high-throughput.
                asio::error_code ec_opt;
                sock->set_option(tcp::socket::receive_buffer_size(m_rcvbuf), ec_opt);
                sock->set_option(tcp::socket::send_buffer_size(m_sndbuf), ec_opt);
                LONG token = m_token_id.fetch_add(1, std::memory_order_relaxed);
                auto sess = std::make_shared<session>(sock, token,
                    [this](LONG t) { remove_session(t); },
                    [this, token](std::vector<std::string>&& batch) { on_session_data(token, std::move(batch)); }
                );
                {
                    std::unique_lock lock(m_sessions_mtx);
                    m_sessions[token] = sess;
                }
                sess->start();
            }
            if (!m_shutdown.load(std::memory_order_acquire)) {
                do_accept();   // repost one accept
            }
        });
}

// ===========================================================================
// main
// ===========================================================================
int main()
{
    try {
        server s("127.0.0.1", 8888);
        std::cout << "Server listening on 127.0.0.1:8888\n";
        std::cout << "Press 'A' to send test message, 'Q' to quit\n";
        int ch = 0;
        do {
            ch = std::getchar();
            ch = std::toupper(ch);
            if ('A' == ch) {
                s.send_message(0, "test1");
                s.send_message(0, "test2");
            }
        } while (ch != 'Q');
    } catch (std::exception& e) {
        std::cerr << "Exception: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
