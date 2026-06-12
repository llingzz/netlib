#include "session.h"
#include "server.h"
#include "protocol.h"
#include "buffer_pool.h"
#include <cstring>

session::session(asio::ip::tcp::socket sock, server& srv, work_queue& wq, buffer_pool& bp, SharedFramePool& fp, const server_config& cfg, uint64_t token)
    : socket_(std::move(sock))
    , server_(srv)
    , wq_(wq)
    , buf_pool_(bp)
    , fpool_(fp)
    , cfg_(cfg)
    , token_(token)
    , read_buf_(bp.acquire())
    , max_payload_(cfg.max_payload)
{
    if (cfg.tcp_nodelay) {
        asio::ip::tcp::no_delay opt(true);
        socket_.set_option(opt);
    }
    // 显式限制 socket 缓冲区, 防止 OS 自动扩到数 MB
    asio::ip::tcp::socket::receive_buffer_size rcv_opt(cfg.socket_rcvbuf);
    asio::ip::tcp::socket::send_buffer_size    snd_opt(cfg.socket_sndbuf);
    asio::error_code ec;
    socket_.set_option(rcv_opt, ec);
    socket_.set_option(snd_opt, ec);
}

session::~session()
{
    if (read_buf_) {
        buf_pool_.release(read_buf_);
    }
}

uint64_t session::token() const
{
    return token_;
}

std::string session::remote_addr() const
{
    asio::error_code ec;
    return socket_.remote_endpoint(ec).address().to_string();
}

void session::start()
{
    do_read();
}

// ──── Read Path ───────────────────────────────────────────
void session::do_read()
{
    auto buf = asio::buffer(read_buf_ + data_len_, buffer_pool::kBufSize - data_len_);
    socket_.async_read_some(buf,
        [this, self = shared_from_this()](auto ec, size_t n) {
            if (ec) {
                if (ec != asio::error::operation_aborted) close();
                return;
            }
            server_.metrics.bytes_received.fetch_add(n, std::memory_order_relaxed);
            process_received_data(n);
            // 读完顺手 flush 写队列: 消除 biz→io 的 post, 读写合并驱动
            try_flush_writes();
            do_read();
        }
    );
}

void session::process_received_data(size_t n)
{
    data_len_ += n;
    if (!cfg_.biz_threads_enabled) {
        // ──── Inline 模式: 直接在 io 线程处理, 零跨线程 ────
        size_t pos = 0;
        while (pos + protocol::HEADER_SIZE <= data_len_) {
            uint16_t plen = protocol::read_header(read_buf_ + pos);
            if (plen == 0 || plen > max_payload_) {
                close();
                return;
            }
            if (pos + protocol::HEADER_SIZE + plen > data_len_) {
                break;
            }
            std::string_view msg(read_buf_ + pos + protocol::HEADER_SIZE, plen);
            server_.handle_message(this, msg);
            pos += protocol::HEADER_SIZE + plen;
        }
        data_len_ -= pos;
        if (data_len_ > 0 && pos > 0) {
            std::memmove(read_buf_, read_buf_ + pos, data_len_);
        }
        // 处理完顺手 flush 写队列
        try_flush_writes();
        return;
    }

    // ──── 分离模式: 批量打包 → work_queue → biz 线程 ────
    size_t pos = 0;
    work_item item;
    item.sess = shared_from_this();
    item.offs.reserve(16);
    while (pos + protocol::HEADER_SIZE <= data_len_) {
        uint16_t plen = protocol::read_header(read_buf_ + pos);
        if (plen == 0 || plen > max_payload_) {
            close();
            return;
        }
        if (pos + protocol::HEADER_SIZE + plen > data_len_) {
            break;
        }
        item.offs.push_back(static_cast<uint16_t>(pos + protocol::HEADER_SIZE));
        item.offs.push_back(plen);
        pos += protocol::HEADER_SIZE + plen;
    }

    char* old_buf = read_buf_;
    if (!item.offs.empty()) {
        item.buf_pool = &buf_pool_;
        item.read_buf = old_buf;
        read_buf_ = buf_pool_.acquire();
    }

    size_t remain = data_len_ - pos;
    if (remain > 0 && pos > 0) {
        std::memcpy(read_buf_, old_buf + pos, remain);
    }
    data_len_ = remain;
    if (!item.offs.empty()) {
        int idx = cfg_.ordered_session ? static_cast<int>(token_ % server_.biz_threads()) : server_.next_wq_idx();
        server_.wq(idx).push(std::move(item));
    }
}

// ──── Write Path  ──

void session::reply(std::string_view data)
{
    if (closed_.load()) {
        return;
    }
    server_.metrics.msg_sent.fetch_add(1, std::memory_order_relaxed);

    char* buf = fpool_.acquire(data.size());
    uint16_t frame_len = protocol::encode_into(data, buf);
    bool need_kick;
    {
        // 背压检查: 写队列超过数量或字节数上限则丢弃帧
        std::lock_guard<std::mutex> lk(write_mtx_);
        if (write_queue_.size() >= cfg_.write_queue_max_count || write_queue_bytes_ + frame_len > cfg_.write_queue_max_bytes) {
            fpool_.release(buf, frame_len);
            server_.metrics.msg_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        need_kick = write_queue_.empty();
        write_queue_.push_back({buf, frame_len});
        write_queue_bytes_ += frame_len;
    }

    if (need_kick) {
        asio::dispatch(socket_.get_executor(),
            [self = shared_from_this()] {
                self->try_flush_writes();
            }
        );
    }
}

void session::try_flush_writes()
{
    if (!write_in_flight_) {
        do_write_kick();
    }
}

void session::do_write_kick()
{
    if (write_in_flight_) {
        return;
    }
    write_in_flight_ = true;

    batch_t batch;
    {
        std::lock_guard<std::mutex> lk(write_mtx_);
        std::swap(batch, write_queue_);
        write_queue_bytes_ = 0;
    }

    if (batch.empty()) {
        write_in_flight_ = false;
        return;
    }
    do_write(std::move(batch));
}

void session::do_write(batch_t batch)
{
    write_bufs_.clear();
    for (auto& fb : batch) {
        write_bufs_.push_back(asio::buffer(fb.data, fb.len));
    }
    asio::async_write(socket_, write_bufs_,
        [this, self = shared_from_this(), batch = std::move(batch)] (auto ec, size_t t) {
            // 无论成功与否，先归还这批次的所有帧缓冲
            for (auto& fb : batch) {
                fpool_.release(fb.data, fb.len);
            }
            if (ec) {
                spdlog::warn("async_write failed: {} token={:#x} bytes_attempted={} wq_remain={}", ec.message(), token_, t, write_queue_.size());
                //   错误路径也必须重置 write_in_flight_ 并排空写队列
                //   否则写链永久死锁，write_queue_ 中的帧缓冲永不归还
                write_in_flight_ = false;
                // 排空积压的写队列，归还所有帧缓冲到池
                drain_write_queue();
                if (ec != asio::error::operation_aborted) {
                    close();
                }
                return;
            }
            server_.metrics.bytes_sent.fetch_add(t, std::memory_order_relaxed);
            write_in_flight_ = false;
            do_write_kick();
        }
    );
}

// 排空写队列：归还所有积压帧缓冲到 SharedFramePool
// 在连接关闭或写链断裂时调用，防止内存泄漏
void session::drain_write_queue()
{
    batch_t leftover;
    {
        std::lock_guard<std::mutex> lk(write_mtx_);
        std::swap(leftover, write_queue_);
        write_queue_bytes_ = 0;
    }
    if (!leftover.empty()) {
        spdlog::warn("drain_write_queue: releasing {} frames token={:#x}", leftover.size(), token_);
        for (auto& fb : leftover) {
            fpool_.release(fb.data, fb.len);
        }
    }
}

// ──── Close ───────────────────────────────────────────────
void session::close()
{
    if (closed_.exchange(true)) return;
    /*spdlog::debug("close() token={:#x} data_len={} wq_size={}", token_, data_len_, write_queue_.size());*/

    // 关闭前排空写队列，归还所有积压帧缓冲，防止池内存泄漏
    drain_write_queue();

    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);

    server_.remove_session(token_);

    if (!cfg_.biz_threads_enabled) {
        // Inline 模式: 直接通知断开
        server_.handle_disconnect(this);
    } else {
        int idx = cfg_.ordered_session ? static_cast<int>(token_ % server_.biz_threads()) : server_.next_wq_idx();
        work_item disc;
        disc.sess = shared_from_this();
        server_.wq(idx).push(std::move(disc));
    }
}
