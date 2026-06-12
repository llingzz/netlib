#include "server.h"
#include "session.h"
#include "buffer_pool.h"
#include <spdlog/spdlog.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <pthread.h>
#endif

server::server(const server_config& cfg, msg_handler on_msg, disconnect_handler on_disc)
    : cfg_(cfg)
    , on_msg_(std::move(on_msg))
    , on_disconnect_(std::move(on_disc))
{
    int io_cnt = std::max<int>(cfg.io_threads  > 0 ? cfg.io_threads : std::thread::hardware_concurrency() * 2 / 3, 1);
    int biz_cnt = std::max<int>(cfg.biz_threads > 0 ? cfg.biz_threads : std::thread::hardware_concurrency() / 3, 1);
    io_ctxs_.reserve(io_cnt);
    for (int i = 0; i < io_cnt; ++i) {
        auto ctx = std::make_unique<io_context>();
        asio::ip::tcp::endpoint ep(asio::ip::make_address(cfg.listen_ip), cfg.listen_port);
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
    if (running_.exchange(true)) {
        return;
    }

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
        // Windows: 单 acceptor
        if (i == 0) {
            do_accept(0);
        }
#else
        // Linux: SO_REUSEPORT 每核 accept
        do_accept(i);
#endif
        io_threads_.emplace_back([this, i] {
            // 线程绑定到对应 CPU 核心，减少调度迁移
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
    // 用 shared_ptr 保存上一轮快照，计算 IO 吞吐速率
    struct Snap { int64_t recv_bytes=0, sent_bytes=0, recv_msgs=0, sent_msgs=0, drops=0; };
    auto prev = std::make_shared<Snap>();
    std::function<void(const asio::error_code&)> on_tick;
    on_tick = [this, &mon_timer, &on_tick, prev](const asio::error_code& ec) {
        if (ec || !running_.load(std::memory_order_relaxed)) {
            return;
        }

        auto buf_bytes   = buf_pool_.total_bytes_allocated();
        auto frame_bytes = frame_pool_.total_bytes_allocated();
        auto buf_count   = buf_pool_.total_allocated();
        auto conns       = metrics.connections.load(std::memory_order_relaxed);

        auto now_recv_bytes = metrics.bytes_received.load(std::memory_order_relaxed);
        auto now_sent_bytes = metrics.bytes_sent.load(std::memory_order_relaxed);
        auto now_recv_msgs  = metrics.msg_received.load(std::memory_order_relaxed);
        auto now_sent_msgs  = metrics.msg_sent.load(std::memory_order_relaxed);
        auto now_drops      = metrics.msg_dropped.load(std::memory_order_relaxed);

        auto dr = now_recv_bytes - prev->recv_bytes;
        auto ds = now_sent_bytes - prev->sent_bytes;
        auto mr = now_recv_msgs  - prev->recv_msgs;
        auto ms = now_sent_msgs  - prev->sent_msgs;
        auto md = now_drops      - prev->drops;

        // 速率: 5 秒间隔 → /5 得每秒
        spdlog::info(
            "[MON] conns:{} | recv: {:.1f} MB/s ({:.0f} msg/s) total:{:.1f} MB | "
            "send: {:.1f} MB/s ({:.0f} msg/s) total:{:.1f} MB | "
            "drop: {} msg/s | "
            "pool: buf={:.1f}MB({}*{}B) frame={:.1f}MB",
            conns,
            dr / (5.0 * 1024*1024), mr / 5.0, now_recv_bytes / (1024.0*1024),
            ds / (5.0 * 1024*1024), ms / 5.0, now_sent_bytes / (1024.0*1024),
            md / 5,
            buf_bytes   / (1024.0 * 1024.0), buf_count, buffer_pool::kBufSize,
            frame_bytes / (1024.0 * 1024.0)
        );

        prev->recv_bytes = now_recv_bytes;
        prev->sent_bytes = now_sent_bytes;
        prev->recv_msgs  = now_recv_msgs;
        prev->sent_msgs  = now_sent_msgs;
        prev->drops      = now_drops;

        mon_timer->expires_after(std::chrono::seconds(5));
        mon_timer->async_wait(on_tick);
    };
    mon_timer->expires_after(std::chrono::seconds(5));
    mon_timer->async_wait(on_tick);

    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
}

void server::shutdown()
{
    if (!running_.exchange(false)) {
        return;
    }
    for (auto& ctx : io_ctxs_) {
        ctx->io_svc.stop();
    }
    for (auto& wq : wq_) {
        wq->stop();
    }
    for (auto& t : biz_pool_) {
        if (t.joinable()) {
            t.join();
        }
    }
    for (auto& t : io_threads_) {
        if (t.joinable()) {
            t.join();
        }
    }
    spdlog::info("Server stopped — msgs: received={} sent={} dropped={} peak_conn={}", metrics.msg_received.load(), metrics.msg_sent.load(), metrics.msg_dropped.load(), metrics.connections.load());
}

void server::do_accept(int io_idx)
{
    auto& io = io_ctxs_[io_idx];
    io->acceptor.async_accept([this, io_idx](asio::error_code ec, asio::ip::tcp::socket sock) {
        if (ec) {
            if (ec != asio::error::operation_aborted) {
                spdlog::error("Accept error[{}]: {}", io_idx, ec.message());
            }
            return;
        }

        auto& io = io_ctxs_[io_idx];
        uint64_t local_id = io->next_local_id++;
        uint64_t token = (static_cast<uint64_t>(io_idx) << 48) | local_id;
        int wq_idx = static_cast<int>(token % wq_.size());
        auto sess = std::make_shared<session>(std::move(sock), *this, *wq_[wq_idx], buf_pool_, frame_pool_, cfg_, token);
        io->sessions[token] = sess;
        sess->start();
        metrics.connections.fetch_add(1, std::memory_order_relaxed);

        do_accept(io_idx);
    });
}

void server::biz_loop(int thread_idx)
{
    // 线程绑定到对应 CPU 核心，减少调度迁移
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
        if (!w.sess) {
            break;
        }
        if (w.offs.empty()) {
            if (on_disconnect_) {
                on_disconnect_(w.sess.get());
            }
            metrics.connections.fetch_sub(1, std::memory_order_relaxed);
            continue;
        }
        metrics.msg_received.fetch_add(w.offs.size() / 2, std::memory_order_relaxed);
        for (size_t i = 0; i < w.offs.size(); i += 2) {
            uint16_t off = w.offs[i];
            uint16_t len = w.offs[i + 1];
            std::string_view msg(w.read_buf + off, len);
            if (on_msg_) {
                on_msg_(w.sess.get(), msg);
            }
        }
        // 处理完归还 read_buf 到池
        if (w.read_buf && w.buf_pool) {
            w.buf_pool->release(w.read_buf);
        }
    }
}

void server::send_message(uint64_t token, std::string msg)
{
    int io_idx = static_cast<int>(token >> 48);
    if (io_idx < 0 || io_idx >= static_cast<int>(io_ctxs_.size())) {
        return;
    }
    asio::post(io_ctxs_[io_idx]->io_svc,
        [this, io_idx, token, msg = std::move(msg)] {
            auto& sessions = io_ctxs_[io_idx]->sessions;
            auto it = sessions.find(token);
            if (it != sessions.end()) {
                it->second->reply(std::move(msg));
            }
        }
    );
}
