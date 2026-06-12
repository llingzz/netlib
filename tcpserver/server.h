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

using msg_handler        = std::function<void(session*, std::string_view)>;
using disconnect_handler = std::function<void(session*)>;

class server {
public:
    server(const server_config& cfg, msg_handler on_msg, disconnect_handler on_disc);
    ~server();

    void run();
    void shutdown();
    void send_message(uint64_t token, std::string msg);

    // 供 session 访问
    work_queue& wq(int idx) { return *wq_[idx]; }
    int biz_threads() const { return static_cast<int>(wq_.size()); }

    // Inline 模式: session 直接调用 handler (io 线程)
    void handle_message(session* sess, std::string_view msg) {
        metrics.msg_received.fetch_add(1, std::memory_order_relaxed);
        if (on_msg_) on_msg_(sess, msg);
    }
    void handle_disconnect(session* sess) {
        metrics.connections.fetch_sub(1, std::memory_order_relaxed);
        if (on_disconnect_) on_disconnect_(sess);
    }

    // 非串行模式: round-robin 选队列
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
    msg_handler         on_msg_;
    disconnect_handler  on_disconnect_;

    std::vector<std::unique_ptr<io_context>> io_ctxs_;
    std::vector<std::thread>   io_threads_;

    std::vector<std::unique_ptr<work_queue>> wq_;
    std::vector<std::thread>   biz_pool_;

    buffer_pool       buf_pool_;
    SharedFramePool   frame_pool_;

public:
    SharedFramePool& frame_pool() { return frame_pool_; }

private:

    std::atomic<bool>    running_{false};
    std::atomic<int>     next_io_ctx_{0};
    std::atomic<int>     next_wq_{0};       // 非串行模式队列分发

public:
    // 内置 metrics
    struct {
        std::atomic<int64_t> msg_received{0};   // 收到的消息总数
        std::atomic<int64_t> msg_sent{0};        // 发出的回复总数
        std::atomic<int64_t> msg_dropped{0};     // 丢弃的消息数
        std::atomic<int64_t> connections{0};     // 当前活跃连接数
        std::atomic<int64_t> bytes_received{ 0 };
        std::atomic<int64_t> bytes_sent{ 0 };
    } metrics;
};
