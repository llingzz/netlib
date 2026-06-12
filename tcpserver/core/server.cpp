#include "server.h"
#include "session.h"
#include "buffer_pool.h"
#include <spdlog/spdlog.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

server::server(const server_config& cfg, data_handler on_data,
               disconnect_handler on_disc, connect_handler on_connect)
    : cfg_(cfg)
    , on_data_(std::move(on_data))
    , on_disconnect_(std::move(on_disc))
    , on_connect_(std::move(on_connect))
{
    int io_cnt = std::max<int>(
        cfg.io_threads > 0 ? cfg.io_threads
                           : std::thread::hardware_concurrency() * 2 / 3,
        1);
    int biz_cnt = std::max<int>(
        cfg.biz_threads > 0 ? cfg.biz_threads
                            : std::thread::hardware_concurrency() / 3,
        1);
    io_ctxs_.reserve(io_cnt);
    for (int i = 0; i < io_cnt; ++i) {
        auto ctx = std::make_unique<io_context>();
        asio::ip::tcp::endpoint ep(
            asio::ip::make_address(cfg.listen_ip), cfg.listen_port);
        ctx->acceptor.open(ep.protocol());
        ctx->acceptor.set_option(asio::ip::tcp::acceptor::reuse_address(true));
#ifndef _WIN32
        {
            asio::detail::socket_option::boolean<SOL_SOCKET, SO_REUSEPORT> opt(true);
            ctx->acceptor.set_option(opt);
        }
#endif
        ctx->acceptor.bind(ep);
        ctx->acceptor.listen(cfg.listen_backlog);
        io_ctxs_.push_back(std::move(ctx));
    }
    wq_.reserve(biz_cnt);
    for (int i = 0; i < biz_cnt; ++i) {
        wq_.push_back(std::make_unique<work_queue>());
    }
    spdlog::info("Server created: {} IO thread(s), {} Biz thread(s)", io_cnt, biz_cnt);
}

server::~server()
{
    shutdown();
}

void server::run()
{
    if (running_.exchange(true)) return;

    biz_pool_.reserve(wq_.size());
    if (cfg_.biz_threads_enabled) {
        for (int i = 0; i < static_cast<int>(wq_.size()); ++i) {
            biz_pool_.emplace_back([this, i] { biz_loop(i); });
        }
        spdlog::info("Biz threads: {} (separate mode)", wq_.size());
    } else {
        spdlog::info("Biz threads: disabled (inline mode)");
    }

    io_threads_.reserve(io_ctxs_.size());
    for (int i = 0; i < static_cast<int>(io_ctxs_.size()); ++i) {
#ifdef _WIN32
        if (i == 0) do_accept(0);
#else
        do_accept(i);
#endif
        io_threads_.emplace_back([this, i] {
            if (cfg_.cpu_affinity) {
#ifdef _WIN32
                SetThreadAffinityMask(GetCurrentThread(), 1ULL << i);
#else
                cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(i, &cs);
                pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
#endif
            }
            io_ctxs_[i]->io_svc.run();
        });
    }

    spdlog::info("Server running on {}:{}", cfg_.listen_ip, cfg_.listen_port);

    // ──── 全局监控定时器 (每 5 秒) ──────────────────
    auto& mon_io = io_ctxs_[0]->io_svc;
    auto mon_timer = std::make_shared<asio::steady_timer>(mon_io);
    struct Snap { int64_t recv_bytes=0, sent_bytes=0, drops=0; };
    auto prev = std::make_shared<Snap>();
    std::function<void(const asio::error_code&)> on_tick;
    on_tick = [this, &mon_timer, &on_tick, prev](const asio::error_code& ec) {
        if (ec || !running_.load(std::memory_order_relaxed)) return;

        auto buf_bytes   = buf_pool_.total_bytes_allocated();
        auto frame_bytes = frame_pool_.total_bytes_allocated();
        auto buf_count   = buf_pool_.total_allocated();
        auto conns       = metrics.connections.load(std::memory_order_relaxed);

        auto now_recv = metrics.bytes_received.load(std::memory_order_relaxed);
        auto now_sent = metrics.bytes_sent.load(std::memory_order_relaxed);
        auto now_drop = metrics.msg_dropped.load(std::memory_order_relaxed);

        auto dr = now_recv - prev->recv_bytes;
        auto ds = now_sent - prev->sent_bytes;
        auto md = now_drop - prev->drops;

        spdlog::info(
            "[MON] conns:{} | recv: {:.1f} MB/s total:{:.1f} MB | "
            "send: {:.1f} MB/s total:{:.1f} MB | "
            "drop: {} | "
            "pool: buf={:.1f}MB({}*{}B) frame={:.1f}MB",
            conns,
            dr / (5.0 * 1024*1024), now_recv / (1024.0*1024),
            ds / (5.0 * 1024*1024), now_sent / (1024.0*1024),
            md,
            buf_bytes   / (1024.0 * 1024.0), buf_count, buffer_pool::kBufSize,
            frame_bytes / (1024.0 * 1024.0)
        );

        prev->recv_bytes = now_recv;
        prev->sent_bytes = now_sent;
        prev->drops      = now_drop;

        mon_timer->expires_after(std::chrono::seconds(5));
        mon_timer->async_wait(on_tick);
    };
    mon_timer->expires_after(std::chrono::seconds(5));
    mon_timer->async_wait(on_tick);

    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
    for (auto& t : biz_pool_) {
        if (t.joinable()) t.join();
    }
    spdlog::info("Server stopped — bytes: recv={} sent={} dropped={} peak_conn={}",
        metrics.bytes_received.load(),
        metrics.bytes_sent.load(),
        metrics.msg_dropped.load(),
        metrics.connections.load());
}

void server::shutdown()
{
    // 仅发送停止信号, 不 join — 可在 io/biz 线程内安全调用
    if (!running_.exchange(false)) return;
    for (auto& ctx : io_ctxs_) ctx->io_svc.stop();
    for (auto& wq : wq_) wq->stop();
}

void server::do_accept(int io_idx)
{
    auto& io = io_ctxs_[io_idx];
    io->acceptor.async_accept([this, io_idx](asio::error_code ec, asio::ip::tcp::socket sock) {
        if (ec) {
            if (ec != asio::error::operation_aborted)
                spdlog::error("Accept error[{}]: {}", io_idx, ec.message());
            return;
        }

        auto& io = io_ctxs_[io_idx];
        uint64_t local_id = io->next_local_id++;
        uint64_t token = (static_cast<uint64_t>(io_idx) << 48) | local_id;
        int wq_idx = static_cast<int>(token % wq_.size());
        auto sess = std::make_shared<session>(
            std::move(sock), *this, *wq_[wq_idx],
            buf_pool_, frame_pool_, cfg_, token);
        io->sessions[token] = sess;

        if (on_connect_ && !on_connect_(sess.get())) {
            io->sessions.erase(token);
            do_accept(io_idx);
            return;
        }

        sess->start();
        metrics.connections.fetch_add(1, std::memory_order_relaxed);
        do_accept(io_idx);
    });
}

void server::biz_loop(int thread_idx)
{
    if (cfg_.cpu_affinity) {
        int core = static_cast<int>(io_ctxs_.size()) + thread_idx;
#ifdef _WIN32
        SetThreadAffinityMask(GetCurrentThread(), 1ULL << core);
#else
        cpu_set_t cpuset; CPU_ZERO(&cpuset); CPU_SET(core, &cpuset);
        pthread_setaffinity_np(pthread_self(), sizeof(cpu_set_t), &cpuset);
#endif
    }

    auto& wq = *wq_[thread_idx];
    while (true) {
        work_item w = wq.pop();
        if (!w.sess) break;             // shutdown

        if (!w.read_buf) {              // disconnect
            if (on_disconnect_) on_disconnect_(w.sess.get());
            continue;
        }

        // 投递原始数据块, 上层负责协议解析
        if (on_data_) {
            on_data_(w.sess.get(), std::string_view(w.read_buf, w.data_len));
        }
        if (w.buf_pool) {
            w.buf_pool->release(w.read_buf);
        }
    }
}

void server::send_data(uint64_t token, const char* data, size_t len)
{
    int io_idx = static_cast<int>(token >> 48);
    if (io_idx < 0 || io_idx >= static_cast<int>(io_ctxs_.size())) return;

    // 拷贝 data 到堆上, 因为 reply_raw 是异步的
    auto buf = std::make_shared<std::string>(data, len);
    asio::post(io_ctxs_[io_idx]->io_svc,
        [this, io_idx, token, buf] {
            auto& sessions = io_ctxs_[io_idx]->sessions;
            auto it = sessions.find(token);
            if (it != sessions.end()) {
                it->second->reply_raw(buf->data(), buf->size());
            }
        }
    );
}
