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
    session(asio::ip::tcp::socket sock, server& srv, work_queue& wq,
            buffer_pool& bp, SharedFramePool& fp,
            const server_config& cfg, uint64_t token);
    ~session();

    uint64_t token() const;
    std::string remote_addr() const;

    void reply(std::string_view data);              // 走 encode 后写入
    void reply_raw(const char* data, size_t len);   // 直接写原始字节

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

    // 读 — 原始字节缓冲
    char*      read_buf_ = nullptr;
    size_t     data_len_ = 0;

    // 写
    std::mutex write_mtx_;
    batch_t    write_queue_;
    size_t     write_queue_bytes_ = 0;
    bool       write_in_flight_ = false;
    std::vector<asio::const_buffer> write_bufs_;

    // 关闭
    std::atomic<bool>       closed_{false};
};
