// tcpserver.h — optimized multi-threaded TCP server
//
// Wire protocol: 8-byte zero-padded ASCII header + payload body.
//
// Optimizations applied:
//   P0: shared_mutex on session map, atomic token id, body length validation
//   P1: close() idempotent guard, graceful shutdown destructor, payload size check
//   P2: direct thread pool (no wrapper thread), dynamic accept,
//       header null-termination, reduced memset, string_view in data handler
//   P3: lock-free MPSC batch queue + raw business thread pool for on_session_data

#pragma once

#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <shared_mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <asio.hpp>

using asio::ip::tcp;

// LONG is Windows SDK type (pulled in via asio → winsock2 → windows.h).
// On Unix platforms, provide an equivalent.
#ifndef _WIN32
using LONG = long;
#endif

// ===========================================================================
// Lock-free bounded MPSC queue — multiple producers (I/O threads), single
// consumer (one business thread). Sequence-based synchronization, no mutex.
// ===========================================================================
template<size_t N>
class MpscQueue {
    static_assert(N > 0 && (N & (N - 1)) == 0, "N must be power of 2");
    static constexpr size_t kMask = N - 1;

    struct Slot {
        std::atomic<uint64_t> seq;
        std::vector<std::string> batch;
    };
    std::array<Slot, N> slots_;
    std::atomic<uint64_t> enqueue_pos_{ 0 };   // CAS by producers
    uint64_t dequeue_pos_{ 0 };                 // consumer only

public:
    MpscQueue() {
        for (size_t i = 0; i < N; ++i) {
            slots_[i].seq.store(i, std::memory_order_relaxed);
        }
    }

    // Returns false if queue is full (should be rare with batched dispatch).
    bool try_enqueue(std::vector<std::string>& batch) {
        uint64_t pos = enqueue_pos_.load(std::memory_order_relaxed);
        for (;;) {
            auto& slot = slots_[pos & kMask];
            uint64_t seq = slot.seq.load(std::memory_order_acquire);
            int64_t diff = (int64_t)seq - (int64_t)pos;
            if (diff == 0) {
                if (enqueue_pos_.compare_exchange_weak(pos, pos + 1, std::memory_order_relaxed)) {
                    slot.batch = std::move(batch);
                    slot.seq.store(pos + 1, std::memory_order_release);
                    return true;
                }
            } else if (diff < 0) {
                return false;   // queue full
            }
            pos = enqueue_pos_.load(std::memory_order_relaxed);
        }
    }

    bool try_dequeue(std::vector<std::string>& out) {
        auto& slot = slots_[dequeue_pos_ & kMask];
        uint64_t seq = slot.seq.load(std::memory_order_acquire);
        int64_t diff = (int64_t)seq - (int64_t)(dequeue_pos_ + 1);
        if (diff == 0) {
            out = std::move(slot.batch);
            slot.seq.store(dequeue_pos_ + N, std::memory_order_release);
            ++dequeue_pos_;
            return true;
        }
        return false;
    }
};

// ---------------------------------------------------------------------------
// session — one TCP connection, async I/O with mutex-protected write queue
// ---------------------------------------------------------------------------
class session : public std::enable_shared_from_this<session>
{
public:
    session(std::shared_ptr<tcp::socket> socket, LONG token,
            const std::function<void(LONG)>& cbRemove,
            const std::function<void(std::vector<std::string>&&)>& cbData);

    void start();
    void close();

    // Thread-safe: can be called from any thread.
    void post_write(std::string_view data);

private:
    // Read path: one async_read fills the buffer, then parse_messages() loops
    // to extract all complete messages in user space. All messages from one
    // read are batched into a single callback dispatch — the I/O thread
    // returns to network work immediately.
    void do_read();

    // Parse all complete messages and dispatch as a single batch.
    void parse_messages();

    void on_data_batch(std::vector<std::string>&& batch);

    // Write path — mutex on write_msgs_, atomic write_in_flight_ prevents
    // concurrent async_write on the same socket.
    void do_write();

    void on_error(asio::error_code ec);
    void try_finish();

    static constexpr int header_len = 8;
    static constexpr size_t read_buf_size = 65536;         // 64KB receive buffer
    static constexpr int max_body = 1024;
    static constexpr size_t write_queue_max_bytes = 65536; // backpressure: max bytes in write queue
    static constexpr size_t max_payload = 99999999;

    std::shared_ptr<tcp::socket> socket_;
    LONG tokenid_;
    std::function<void(LONG)> cb_remove_;
    std::function<void(std::vector<std::string>&&)> cb_on_data_;

    std::array<char, read_buf_size> read_buf_{};
    size_t data_len_ = 0;

    std::mutex write_mtx_;
    std::deque<std::string> write_msgs_;
    size_t write_queue_bytes_ = 0;
    std::string batch_buf_;

    std::atomic<bool> write_in_flight_{ false };
    std::atomic<bool> read_eof_{ false };
    std::atomic<bool> closed_{ false };
    std::atomic<int> request_count_{ 0 };
};

// ---------------------------------------------------------------------------
// server — acceptor + session management
// ---------------------------------------------------------------------------
class server
{
public:
    server(std::string ip, int port, int rcvbuf = 65536, int sndbuf = 65536);
    ~server();

    void shutdown();
    void send_message(LONG token, const std::string& msg);
    void remove_session(LONG token);

private:
    virtual std::string deserialize_data(std::string_view data);

    // Called on I/O thread — just enqueue the batch, return immediately.
    virtual void on_session_data(LONG token, std::vector<std::string>&& batch);

    // Runs on business thread.
    void process_batch(std::vector<std::string>& batch);

    void do_accept();

    // --- members ---
    std::atomic<bool> m_shutdown{ false };
    std::atomic<LONG> m_token_id{ 0 };

    // I/O
    std::unique_ptr<asio::io_service> m_io_context;
    std::unique_ptr<tcp::acceptor> m_acceptor;
    std::vector<std::thread> m_work_threads;

    // Business thread pool with lock-free queues.
    // Same token → same queue → strict FIFO serialization per token.
    std::vector<std::unique_ptr<MpscQueue<256>>> m_biz_queues;
    std::vector<std::thread> m_biz_threads;
    std::atomic<bool> m_biz_stop{ false };

    int m_rcvbuf;
    int m_sndbuf;
    std::shared_mutex m_sessions_mtx;
    std::map<LONG, std::shared_ptr<session>> m_sessions;
};
