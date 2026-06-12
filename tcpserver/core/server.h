#pragma once
#include <asio.hpp>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <thread>
#include <unordered_map>
#include <vector>
#include "config.h"
#include "work_queue.h"
#include "buffer_pool.h"
#include "shared_frame_pool.h"

class session;

using data_handler        = std::function<void(session*, std::string_view)>;
using disconnect_handler  = std::function<void(session*)>;
using connect_handler     = std::function<bool(session*)>;

class server {
public:
    server(const server_config& cfg, data_handler on_data,
           disconnect_handler on_disc,
           connect_handler on_connect = nullptr);
    ~server();

    void run();
    void shutdown();
    void send_data(uint64_t token, const char* data, size_t len);

    // 供 session 访问
    work_queue& wq(int idx) { return *wq_[idx]; }
    int biz_threads() const { return static_cast<int>(wq_.size()); }

    // Inline: session 直接回调 (io 线程)
    void handle_data(session* sess, std::string_view data) {
        if (on_data_) on_data_(sess, data);
    }
    void handle_disconnect(session* sess) {
        metrics.connections.fetch_sub(1, std::memory_order_relaxed);
        if (on_disconnect_) on_disconnect_(sess);
    }

    int next_wq_idx() {
        int n = next_wq_.fetch_add(1, std::memory_order_relaxed);
        return n % static_cast<int>(wq_.size());
    }
    asio::io_context& io_ctx(int idx) { return io_ctxs_[idx]->io_svc; }
    uint64_t next_token(int io_idx) { return io_ctxs_[io_idx]->next_local_id++; }
    void remove_session(uint64_t token) {
        int io_idx = static_cast<int>(token >> 48);
        io_ctxs_[io_idx]->sessions.erase(token);
    }

    server_config& config() { return cfg_; }
    const server_config& config() const { return cfg_; }
    SharedFramePool& frame_pool() { return frame_pool_; }

private:
    void do_accept(int io_idx);
    void biz_loop(int thread_idx);

    struct io_context {
        asio::io_context io_svc;
        asio::executor_work_guard<asio::io_context::executor_type> work;
        asio::ip::tcp::acceptor acceptor;
        std::unordered_map<uint64_t, std::shared_ptr<session>> sessions;
        uint64_t next_local_id = 0;

        io_context()
            : work(asio::make_work_guard(io_svc))
            , acceptor(io_svc) {}
    };

    server_config       cfg_;
    data_handler        on_data_;
    disconnect_handler  on_disconnect_;
    connect_handler     on_connect_;

    std::vector<std::unique_ptr<io_context>> io_ctxs_;
    std::vector<std::thread>   io_threads_;

    std::vector<std::unique_ptr<work_queue>> wq_;
    std::vector<std::thread>   biz_pool_;

    buffer_pool       buf_pool_;
    SharedFramePool   frame_pool_;

    std::atomic<bool>    running_{false};
    std::atomic<int>     next_io_ctx_{0};
    std::atomic<int>     next_wq_{0};

public:
    // 内置 metrics (纯字节/连接粒度 — "消息"概念在 framework 层)
    struct {
        std::atomic<int64_t> msg_dropped{0};      // 背压丢弃数
        std::atomic<int64_t> connections{0};       // 当前活跃连接数
        std::atomic<int64_t> bytes_received{0};
        std::atomic<int64_t> bytes_sent{0};
    } metrics;
};
