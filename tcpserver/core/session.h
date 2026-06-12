#pragma once
#include <asio.hpp>
#include <atomic>
#include <cstddef>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include "config.h"

class server;
class work_queue;
class buffer_pool;
class SharedFramePool;

struct frame_buf {
    char* data = nullptr;
    uint16_t len = 0;
};

class session : public std::enable_shared_from_this<session>
{
public:
    session(asio::ip::tcp::socket sock, server& srv, work_queue& wq, buffer_pool& bp, SharedFramePool& fp, const server_config& cfg, uint64_t token);
    ~session();

    uint64_t token() const;

    // 对端地址 (用于 IP 过滤等)
    std::string remote_addr() const;

    void reply(std::string_view data);
    void start();
    void close();
    void try_flush_writes();

private:
    void do_read();
    void process_received_data(size_t n);
    void do_write_kick();
    void do_write(std::deque<frame_buf> batch);
    void drain_write_queue();

    using batch_t = std::deque<frame_buf>;

    asio::ip::tcp::socket   socket_;
    server&                 server_;
    work_queue&             wq_;
    buffer_pool&            buf_pool_;
    SharedFramePool&        fpool_;
    const server_config&    cfg_;
    uint64_t                token_;

    // 读
    char*      read_buf_ = nullptr;
    size_t     data_len_ = 0;

    // 写
    std::mutex write_mtx_;
    batch_t    write_queue_;
    size_t     write_queue_bytes_ = 0;
    bool       write_in_flight_ = false;
    std::vector<asio::const_buffer> write_bufs_;  // 复用

    // 关闭
    std::atomic<bool>       closed_{false};

    uint16_t max_payload_;
};
