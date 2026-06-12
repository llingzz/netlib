#include "session.h"
#include "server.h"
#include "buffer_pool.h"
#include <cstring>
#include <spdlog/spdlog.h>

session::session(asio::ip::tcp::socket sock, server& srv, work_queue& wq,
                 buffer_pool& bp, SharedFramePool& fp,
                 const server_config& cfg, uint64_t token)
    : socket_(std::move(sock))
    , server_(srv)
    , wq_(wq)
    , buf_pool_(bp)
    , fpool_(fp)
    , cfg_(cfg)
    , token_(token)
    , read_buf_(bp.acquire())
{
    if (cfg.tcp_nodelay) {
        asio::ip::tcp::no_delay opt(true);
        socket_.set_option(opt);
    }
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
            try_flush_writes();
            do_read();
        }
    );
}

void session::process_received_data(size_t n)
{
    data_len_ += n;

    // ──── 投递原始数据块, 上层负责协议解析 ────
    std::string_view raw(read_buf_, data_len_);

    if (!cfg_.biz_threads_enabled) {
        // Inline: 直接回调
        server_.handle_data(this, raw);
        data_len_ = 0;
        try_flush_writes();
        return;
    }

    // Separate: 打包 work_item, 移交 read_buf_
    work_item item;
    item.sess     = shared_from_this();
    item.buf_pool = &buf_pool_;
    item.read_buf = read_buf_;
    item.data_len = data_len_;

    read_buf_ = buf_pool_.acquire();
    data_len_ = 0;

    int idx = cfg_.ordered_session
        ? static_cast<int>(token_ % server_.biz_threads())
        : server_.next_wq_idx();
    server_.wq(idx).push(std::move(item));
}

// ──── Write Path ──

void session::reply(std::string_view data)
{
    // 兼容接口: 上层调用 reply() 时会走 codec encode
    // 但 codec 在 framework 层, session 不持有。
    // 这里仅作为占位: 业务代码应通过 tcp_service::reply() 发送,
    // 它会调 codec_->encode() 后调 reply_raw()。
    // 如果直接调 session::reply(), 数据原样写入 (等同于 raw)。
    reply_raw(data.data(), data.size());
}

void session::reply_raw(const char* data, size_t len)
{
    if (closed_.load()) {
        return;
    }

    if (len == 0) return;

    char* buf = fpool_.acquire(len);
    std::memcpy(buf, data, len);

    bool need_kick;
    {
        std::lock_guard<std::mutex> lk(write_mtx_);
        if (write_queue_.size() >= cfg_.write_queue_max_count
            || write_queue_bytes_ + len > cfg_.write_queue_max_bytes) {
            fpool_.release(buf, len);
            server_.metrics.msg_dropped.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        need_kick = write_queue_.empty();
        write_queue_.push_back({buf, static_cast<uint16_t>(len)});
        write_queue_bytes_ += len;
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
    if (write_in_flight_) return;
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
        [this, self = shared_from_this(), batch = std::move(batch)]
        (auto ec, size_t t) {
            for (auto& fb : batch) {
                fpool_.release(fb.data, fb.len);
            }
            if (ec) {
                spdlog::warn("async_write failed: {} token={:#x} bytes_attempted={} wq_remain={}",
                    ec.message(), token_, t, write_queue_.size());
                write_in_flight_ = false;
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

void session::drain_write_queue()
{
    batch_t leftover;
    {
        std::lock_guard<std::mutex> lk(write_mtx_);
        std::swap(leftover, write_queue_);
        write_queue_bytes_ = 0;
    }
    if (!leftover.empty()) {
        spdlog::warn("drain_write_queue: releasing {} frames token={:#x}",
            leftover.size(), token_);
        for (auto& fb : leftover) {
            fpool_.release(fb.data, fb.len);
        }
    }
}

// ──── Close ───────────────────────────────────────────────
void session::close()
{
    if (closed_.exchange(true)) return;

    drain_write_queue();

    asio::error_code ec;
    socket_.shutdown(asio::ip::tcp::socket::shutdown_both, ec);
    socket_.close(ec);

    server_.remove_session(token_);

    if (!cfg_.biz_threads_enabled) {
        server_.handle_disconnect(this);
    } else {
        int idx = cfg_.ordered_session
            ? static_cast<int>(token_ % server_.biz_threads())
            : server_.next_wq_idx();
        work_item disc;
        disc.sess = shared_from_this();
        // read_buf == nullptr → 断开信号
        server_.wq(idx).push(std::move(disc));
    }
}
