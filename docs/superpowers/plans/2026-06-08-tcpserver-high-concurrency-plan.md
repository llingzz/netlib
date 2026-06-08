# TCP Server 高并发改造 实现计划

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** 将 tcpserver 从单 io_service + 全局锁架构改造为三层分离的高并发架构（Tier 3: 50万连接, 200万 msg/s）

**Architecture:** IO 层 (K 个 per-core io_service + strand per connection) → 消息总线 (per-session SPSC ring buffer + 全局 ready queue) → 业务层 (J 个 work-stealing 业务线程)。去除 boost 依赖，scatter-gather 写，分片 session map。

**Tech Stack:** C++17, asio (standalone), spdlog, MSBuild v143, vcpkg manifest-mode

---

## 文件结构

```
tcpserver/
├── spsc_ring.h               # 创建 — SPSC ring buffer (header-only)
├── ready_queue.h             # 创建 — 全局就绪队列 (header-only)
├── server_config.h           # 创建 — 配置结构体
├── business_processor.h      # 创建 — 业务处理器接口
├── sharded_map.h             # 创建 — 分片 map (header-only)
├── session.h                 # 创建 — session 类声明
├── session.cpp               # 创建 — session 类实现
├── server.h                  # 创建 — server 类声明
├── server.cpp                # 创建 — server 类实现
├── main.cpp                  # 创建 — 入口
├── tcpserver.cpp             # 删除 — 被上述文件替代
└── tcpserver.vcxproj         # 修改 — 添加新文件引用
```

---

### Task 1: `spsc_ring.h` — Lock-free SPSC Ring Buffer

**Files:**
- Create: `tcpserver/spsc_ring.h`

- [ ] **Step 1: Write spsc_ring.h**

```cpp
#pragma once
#include <atomic>
#include <vector>
#include <cstddef>

// Lock-free SPSC (Single Producer, Single Consumer) ring buffer.
//
// Producer (one thread):  enqueue() + empty() before enqueue to detect wakeup need
// Consumer (one thread):  dequeue() + available()
//
// Uses acquire/release ordering — no CAS needed for SPSC.

class spsc_ring {
public:
    // capacity is rounded up to the next power of 2
    explicit spsc_ring(size_t capacity) {
        size_t cap = 1;
        while (cap < capacity) cap <<= 1;
        buf_.resize(cap);
        mask_ = cap - 1;
    }

    // Producer: push one item. Returns false if full.
    bool enqueue(std::string item) {
        size_t h = head_.load(std::memory_order_relaxed);
        size_t t = tail_.load(std::memory_order_acquire);
        if (h - t >= buf_.size()) return false;
        buf_[h & mask_] = std::move(item);
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    // Consumer: pop one item. Returns false if empty.
    bool dequeue(std::string& out) {
        size_t t = tail_.load(std::memory_order_relaxed);
        size_t h = head_.load(std::memory_order_acquire);
        if (t == h) return false;
        out = std::move(buf_[t & mask_]);
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    // Consumer: number of items available to dequeue
    size_t available() const {
        size_t h = head_.load(std::memory_order_acquire);
        size_t t = tail_.load(std::memory_order_relaxed);
        return h - t;
    }

    bool empty() const { return head_.load(std::memory_order_acquire) == tail_.load(std::memory_order_relaxed); }

    size_t capacity() const { return buf_.size(); }

private:
    alignas(64) std::atomic<size_t> head_{0};   // producer writes
    alignas(64) std::atomic<size_t> tail_{0};   // consumer writes
    alignas(64) std::vector<std::string> buf_;  // padding after to isolate from adjacent hot fields
    size_t mask_ = 0;
};
```

- [ ] **Step 2: Verify compilation**

```bash
cd "D:\Work\Server\netlib" && cl /std:c++17 /c /EHsc tcpserver/spsc_ring.h /Fe:tcpserver/spsc_ring_test.obj 2>&1 || echo "Header-only file — syntax check passed if no errors above"
```

- [ ] **Step 3: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/spsc_ring.h && git commit -m "feat: add lock-free SPSC ring buffer

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 2: `ready_queue.h` — 全局就绪队列

**Files:**
- Create: `tcpserver/ready_queue.h`

- [ ] **Step 1: Write ready_queue.h**

```cpp
#pragma once
#include <deque>
#include <mutex>
#include <condition_variable>
#include <atomic>

// Global ready queue — multiple IO strands produce, multiple business threads consume.
// Each entry is a session token (LONG). Uses mutex + condition_variable for simplicity;
// push rate is bounded (only when per-session ring transitions empty→non-empty).

class ready_queue {
public:
    explicit ready_queue(size_t max_size = 65536) : max_size_(max_size) {}

    // Push a token. Returns false if queue is full.
    bool push(LONG token) {
        std::lock_guard<std::mutex> lock(mtx_);
        if (queue_.size() >= max_size_) return false;
        queue_.push_back(token);
        cv_.notify_one();
        return true;
    }

    // Pop a token. Blocks until available or shutdown. Returns 0 on shutdown.
    LONG pop() {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this] { return !queue_.empty() || shutdown_.load(std::memory_order_acquire); });
        if (queue_.empty()) return 0;
        LONG token = queue_.front();
        queue_.pop_front();
        return token;
    }

    // Wake all waiters — used during shutdown
    void shutdown() {
        shutdown_.store(true, std::memory_order_release);
        cv_.notify_all();
    }

private:
    std::deque<LONG> queue_;
    std::mutex mtx_;
    std::condition_variable cv_;
    std::atomic<bool> shutdown_{false};
    size_t max_size_;
};
```

- [ ] **Step 2: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/ready_queue.h && git commit -m "feat: add global ready queue for business thread notification

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 3: `server_config.h` — 配置结构体

**Files:**
- Create: `tcpserver/server_config.h`

- [ ] **Step 1: Write server_config.h**

```cpp
#pragma once
#include <string>
#include <spdlog\spdlog.h>

struct server_config {
    // --- 监听 ---
    std::string listen_ip = "0.0.0.0";
    int listen_port = 8888;

    // --- IO 层 ---
    int io_threads = 0;          // 0 = auto: hardware_concurrency * 2/3, min 1
    int strands_per_io = 64;     // 每个 io_service 的 strand 数量
    int socket_rcvbuf = 65536;
    int socket_sndbuf = 65536;
    bool tcp_nodelay = true;

    // --- 业务层 ---
    int biz_threads = 0;         // 0 = auto: hardware_concurrency * 1/3, min 1

    // --- 消息总线 ---
    int ring_buffer_capacity = 1024;   // per-session ring buffer 槽数
    int ready_queue_capacity = 65536;  // 全局就绪队列容量
    int max_payload = 65536;           // 单条消息最大字节数

    // --- 写路径 ---
    int write_queue_max_bytes = 262144;  // per session 写队列最大字节数
    int write_queue_max_count = 1024;    // 最大排队消息数
    int read_buf_size = 65536;           // 接收缓冲区大小

    // --- Session 管理 ---
    int session_map_shards = 256;   // 分片数 (2的幂)

    // --- 通用 ---
    spdlog::level::level_enum log_level = spdlog::level::info;
    bool cpu_affinity = false;
};
```

- [ ] **Step 2: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/server_config.h && git commit -m "feat: add server configuration struct

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 4: `business_processor.h` — 业务处理器接口

**Files:**
- Create: `tcpserver/business_processor.h`

- [ ] **Step 1: Write business_processor.h**

```cpp
#pragma once
#include <string>
#include <string_view>

// Abstract business processor — application logic runs here.
// All methods run on business threads, can be long-running.
// Each method receives the session token for context.

class business_processor {
public:
    virtual ~business_processor() = default;

    // Process one complete message payload (header already stripped).
    // Return empty string to send no response, or a payload to echo back.
    virtual std::string on_message(LONG token, std::string_view data) = 0;

    // Called when a new connection is established (optional override).
    virtual void on_connect(LONG token) {}

    // Called when a connection is closing, after all pending messages
    // in the ring buffer have been processed (optional override).
    virtual void on_disconnect(LONG token) {}
};

// Default echo processor — returns the received data as-is
class echo_processor : public business_processor {
public:
    std::string on_message(LONG token, std::string_view data) override {
        return std::string(data);
    }
};
```

- [ ] **Step 2: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/business_processor.h && git commit -m "feat: add business processor interface with echo default

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 5: `sharded_map.h` — 分片 Session Map

**Files:**
- Create: `tcpserver/sharded_map.h`

- [ ] **Step 1: Write sharded_map.h**

```cpp
#pragma once
#include <array>
#include <shared_mutex>
#include <unordered_map>
#include <memory>
#include <functional>

// Sharded concurrent map for session storage.
// ShardCount must be a power of 2. Each shard has its own rwlock.
// K = key (LONG), V = value (session)

template<typename K, typename V, size_t ShardCount = 256>
class sharded_map {
    static_assert((ShardCount & (ShardCount - 1)) == 0, "ShardCount must be a power of 2");

    struct shard {
        std::shared_mutex mtx;
        std::unordered_map<K, std::shared_ptr<V>> map;
    };

    std::array<shard, ShardCount> shards_;

    static constexpr size_t shard_idx(K key) {
        return std::hash<K>{}(key) & (ShardCount - 1);
    }

public:
    // Returns shared_ptr (nullptr if not found). Thread-safe, shared lock.
    std::shared_ptr<V> find(K key) {
        auto& s = shards_[shard_idx(key)];
        std::shared_lock lock(s.mtx);
        auto it = s.map.find(key);
        return (it != s.map.end()) ? it->second : nullptr;
    }

    // Insert or replace. Thread-safe, exclusive lock.
    void insert(K key, std::shared_ptr<V> val) {
        auto& s = shards_[shard_idx(key)];
        std::unique_lock lock(s.mtx);
        s.map[key] = std::move(val);
    }

    // Remove by key. Thread-safe, exclusive lock.
    void erase(K key) {
        auto& s = shards_[shard_idx(key)];
        std::unique_lock lock(s.mtx);
        s.map.erase(key);
    }
};
```

- [ ] **Step 2: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/sharded_map.h && git commit -m "feat: add sharded concurrent map for session storage

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 6: `session.h` — Session 类声明

**Files:**
- Create: `tcpserver/session.h`

- [ ] **Step 1: Write session.h**

```cpp
#pragma once
#include <array>
#include <atomic>
#include <deque>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <asio.hpp>
#include "spsc_ring.h"
#include "ready_queue.h"
#include "server_config.h"

using asio::ip::tcp;

class session : public std::enable_shared_from_this<session> {
public:
    session(tcp::socket socket,
            asio::io_service::strand& strand,
            LONG token,
            std::function<void(LONG)> cb_remove,
            ready_queue& rq,
            const server_config& cfg);

    void start();
    void close();

    // Thread-safe — called from any thread (business or external).
    // Frames payload with 8-byte header and dispatches to IO strand.
    void post_write(const std::string& data);

    LONG token() const { return tokenid_; }
    spsc_ring& ring() { return ring_; }
    bool biz_closed() const { return biz_closed_.load(std::memory_order_acquire); }

private:
    // --- Read path ---
    void do_read();
    void parse_messages();

    // Push a complete message payload to the ring buffer.
    // If ring was empty, notify the business thread via ready_queue.
    void on_data(std::string_view data);

    // --- Write path (all run on strand, no mutex) ---
    void enqueue_write(std::string frame, size_t frame_bytes);
    void do_write();

    // --- Lifecycle ---
    void on_error(asio::error_code ec);
    void try_finish();

    // --- Constants ---
    static constexpr int header_len = 8;

    // --- Members ---
    tcp::socket socket_;
    asio::io_service::strand& strand_;
    LONG tokenid_;
    std::function<void(LONG)> cb_remove_;

    // Message bus
    spsc_ring ring_;                    // IO→Biz SPSC channel
    ready_queue& ready_queue_;          // Global ready queue for notification

    // Read buffer (embedded, 64KB typical)
    std::vector<char> read_buf_;
    size_t data_len_ = 0;

    // Write queue — strand context only
    std::deque<std::string> write_msgs_;
    size_t write_queue_bytes_ = 0;
    std::vector<asio::const_buffer> scatter_bufs_;  // reused for gather-write

    // Config ref
    const server_config& cfg_;

    // State
    bool read_eof_ = false;
    std::atomic<bool> closed_{false};
    std::atomic<bool> biz_closed_{false};
};
```

- [ ] **Step 2: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/session.h && git commit -m "feat: add session class declaration

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 7: `session.cpp` — Session 类实现

**Files:**
- Create: `tcpserver/session.cpp`

- [ ] **Step 1: Write session.cpp constructor + start**

```cpp
#include "session.h"
#include <charconv>
#include <cstring>
#include <spdlog\spdlog.h>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
// ─────────────────────────────────────────────────────────────────────────────
session::session(tcp::socket socket,
                 asio::io_service::strand& strand,
                 LONG token,
                 std::function<void(LONG)> cb_remove,
                 ready_queue& rq,
                 const server_config& cfg)
    : socket_(std::move(socket))
    , strand_(strand)
    , tokenid_(token)
    , cb_remove_(std::move(cb_remove))
    , ring_(cfg.ring_buffer_capacity)
    , ready_queue_(rq)
    , read_buf_(cfg.read_buf_size)
    , cfg_(cfg)
{
}

void session::start() {
    do_read();
}
```

- [ ] **Step 2: Write read path (do_read + parse_messages + on_data)**

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// Read path — one async_read, then parse_messages() extracts all complete frames
// ─────────────────────────────────────────────────────────────────────────────
void session::do_read() {
    auto self = shared_from_this();
    asio::async_read(socket_,
        asio::buffer(read_buf_.data() + data_len_, read_buf_.size() - data_len_),
        asio::bind_executor(strand_,
            [this, self](asio::error_code ec, std::size_t bytes_read) {
                if (ec) {
                    if (ec == asio::error::eof) {
                        // Client closed write side (FIN) — process remaining,
                        // discard incomplete residual, wait for writes to drain.
                        if (data_len_ > 0) {
                            parse_messages();
                            data_len_ = 0;
                        }
                        read_eof_ = true;
                        try_finish();
                    } else {
                        on_error(ec);
                    }
                    return;
                }
                data_len_ += bytes_read;
                parse_messages();
                do_read();
            }
        ));
}

void session::parse_messages() {
    size_t pos = 0;
    while (pos + header_len <= data_len_) {
        int body_len = 0;
        auto fc = std::from_chars(
            read_buf_.data() + pos,
            read_buf_.data() + pos + header_len,
            body_len);
        if (fc.ec != std::errc() || body_len <= 0 || body_len > cfg_.max_payload) {
            spdlog::warn("client {} parse error, closing", tokenid_);
            close();
            return;
        }
        size_t msg_len = header_len + static_cast<size_t>(body_len);
        if (pos + msg_len > data_len_) {
            break;  // incomplete frame, wait for next read
        }
        on_data(std::string_view(
            read_buf_.data() + pos + header_len,
            static_cast<size_t>(body_len)));
        pos += msg_len;
    }
    // Compact residual to buffer front
    if (pos > 0 && pos < data_len_) {
        std::memmove(read_buf_.data(), read_buf_.data() + pos, data_len_ - pos);
        data_len_ -= pos;
    } else if (pos == data_len_) {
        data_len_ = 0;
    }
}

void session::on_data(std::string_view data) {
    // Copy payload to string for ring buffer ownership
    bool was_empty = ring_.empty();
    if (!ring_.enqueue(std::string(data))) {
        spdlog::warn("client {} ring buffer full, dropping message", tokenid_);
        return;
    }
    // Notify business thread only on empty→non-empty transition
    if (was_empty) {
        ready_queue_.push(tokenid_);
    }
}
```

- [ ] **Step 3: Write write path (post_write + enqueue_write + do_write)**

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// Write path — strand-serialized, lock-free, scatter-gather for batching
// ─────────────────────────────────────────────────────────────────────────────
void session::post_write(const std::string& data) {
    if (data.size() > static_cast<size_t>(cfg_.max_payload)) {
        return;
    }
    // Format: 8-byte zero-padded header + payload
    char head[16];
    snprintf(head, sizeof(head), "%08d", static_cast<int>(data.size()));
    std::string frame = std::string(head, header_len) + data;
    size_t frame_bytes = header_len + data.size();

    auto self = shared_from_this();
    strand_.post([this, self, frame = std::move(frame), frame_bytes]() mutable {
        enqueue_write(std::move(frame), frame_bytes);
    });
}

void session::enqueue_write(std::string frame, size_t frame_bytes) {
    if (write_queue_bytes_ + frame_bytes > static_cast<size_t>(cfg_.write_queue_max_bytes) ||
        write_msgs_.size() >= static_cast<size_t>(cfg_.write_queue_max_count)) {
        spdlog::warn("client {} write queue full, dropping message", tokenid_);
        return;
    }
    bool was_empty = write_msgs_.empty();
    write_queue_bytes_ += frame_bytes;
    write_msgs_.push_back(std::move(frame));
    if (was_empty) {
        do_write();
    }
}

void session::do_write() {
    auto self = shared_from_this();
    size_t count = write_msgs_.size();

    if (count == 1) {
        // Fast path: single message, send directly
        asio::async_write(socket_, asio::buffer(write_msgs_.front()),
            asio::bind_executor(strand_,
                [this, self](asio::error_code ec, std::size_t) {
                    if (ec) { on_error(ec); return; }
                    write_queue_bytes_ -= write_msgs_.front().size();
                    write_msgs_.pop_front();
                    if (!write_msgs_.empty()) do_write();
                    else try_finish();
                }));
    } else {
        // Batch path: scatter-gather, zero-copy (writev / WSASend)
        scatter_bufs_.clear();
        scatter_bufs_.reserve(count);
        for (auto& msg : write_msgs_) {
            scatter_bufs_.push_back(asio::buffer(msg));
        }
        asio::async_write(socket_, scatter_bufs_,
            asio::bind_executor(strand_,
                [this, self, count](asio::error_code ec, std::size_t) {
                    if (ec) { on_error(ec); return; }
                    for (size_t i = 0; i < count; ++i) {
                        write_queue_bytes_ -= write_msgs_.front().size();
                        write_msgs_.pop_front();
                    }
                    if (!write_msgs_.empty()) do_write();
                    else try_finish();
                }));
    }
}
```

- [ ] **Step 4: Write lifecycle (close + on_error + try_finish)**

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void session::close() {
    if (closed_.exchange(true)) {
        return;  // idempotent
    }
    biz_closed_.store(true, std::memory_order_release);

    // Always wake business thread for on_disconnect cleanup.
    // Even if ring has data, the business thread needs to know we're closing.
    // (Duplicate pushes to ready_queue are harmless — business thread handles idempotently.)
    ready_queue_.push(tokenid_);

    asio::error_code ignored;
    socket_.shutdown(tcp::socket::shutdown_both, ignored);
    socket_.close(ignored);

    if (cb_remove_) {
        cb_remove_(tokenid_);
    }
}

void session::on_error(asio::error_code ec) {
    if (ec == asio::error::connection_reset) {
        // RST from client — silent
    } else if (ec != asio::error::operation_aborted) {
        // Unexpected error
    }
    close();
}

void session::try_finish() {
    if (read_eof_ && write_msgs_.empty()) {
        close();
    }
}
```

- [ ] **Step 5: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/session.cpp && git commit -m "feat: add session class implementation

- Read path: single async_read + parse_messages loop
- Write path: scatter-gather batch write (zero-copy)
- SPSC ring buffer for IO→Biz messaging
- Ready queue notification on empty→non-empty transition

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 8: `server.h` — Server 类声明

**Files:**
- Create: `tcpserver/server.h`

- [ ] **Step 1: Write server.h**

```cpp
#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <asio.hpp>
#include "server_config.h"
#include "sharded_map.h"
#include "ready_queue.h"
#include "business_processor.h"

using asio::ip::tcp;

class session;

class server {
public:
    // Construct and start the server. If processor is null, no business
    // processing is done (messages are received but not acted upon).
    server(const server_config& cfg,
           std::unique_ptr<business_processor> processor = nullptr);

    ~server();

    // Send a message to a specific session. Thread-safe.
    void send_message(LONG token, const std::string& msg);

    // Graceful shutdown — close acceptor, drain sessions, join threads.
    void shutdown();

private:
    // Per-IO-core data
    struct io_core {
        std::unique_ptr<asio::io_service> svc;
        std::unique_ptr<tcp::acceptor> acceptor;  // only core[0] on Windows
        std::vector<asio::io_service::strand> strands;
        std::thread thread;
    };

    void do_accept(size_t core_idx);
    void business_thread_func();
    void remove_session(LONG token);

    server_config cfg_;
    std::unique_ptr<business_processor> processor_;
    ready_queue ready_queue_;

    // IO cores
    std::vector<io_core> io_cores_;
    std::vector<std::thread> io_threads_;  // Windows: multiple threads on single io_service

    // Business threads
    std::vector<std::thread> biz_threads_;
    std::atomic<bool> biz_shutdown_{false};

    // Session storage
    sharded_map<LONG, session, 256> sessions_;

    // Shutdown guard
    std::atomic<bool> shutdown_{false};
    std::atomic<LONG> next_token_{0};
};
```

- [ ] **Step 2: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/server.h && git commit -m "feat: add server class declaration

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 9: `server.cpp` — Server 类实现

**Files:**
- Create: `tcpserver/server.cpp`

- [ ] **Step 1: Write server.cpp — constructor**

```cpp
#include "server.h"
#include "session.h"
#include <iostream>
#include <spdlog\spdlog.h>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor — initialize IO cores, business threads, and acceptors
// ─────────────────────────────────────────────────────────────────────────────
server::server(const server_config& cfg,
               std::unique_ptr<business_processor> processor)
    : cfg_(cfg)
    , processor_(std::move(processor))
    , ready_queue_(cfg.ready_queue_capacity)
{
    // Resolve address once
    asio::io_service tmp_svc;
    tcp::resolver resolver(tmp_svc);
    auto endpoints = resolver.resolve(cfg.listen_ip, std::to_string(cfg.listen_port));
    tcp::endpoint endpoint(*endpoints);

    // IO cores: one per core on Linux (SO_REUSEPORT), single on Windows
#ifdef __linux__
    size_t n_io = cfg.io_threads > 0
        ? static_cast<size_t>(cfg.io_threads)
        : std::max(1u, std::thread::hardware_concurrency() * 2 / 3);
#else
    // Windows: single io_service with multiple threads (IOCP handles multi-core)
    size_t n_io = 1;
    size_t n_io_threads = cfg.io_threads > 0
        ? static_cast<size_t>(cfg.io_threads)
        : std::max(1u, std::thread::hardware_concurrency());
#endif
    spdlog::info("Starting {} IO core(s)", n_io);

    io_cores_.resize(n_io);

    // Create IO cores
    for (size_t i = 0; i < n_io; ++i) {
        auto& core = io_cores_[i];
        core.svc = std::make_unique<asio::io_service>();

        // Create strands per io_service
        core.strands.reserve(cfg.strands_per_io);
        for (int s = 0; s < cfg.strands_per_io; ++s) {
            core.strands.emplace_back(*core.svc);
        }

        // Acceptor: per-core on Linux (SO_REUSEPORT), single on Windows
        core.acceptor = std::make_unique<tcp::acceptor>(*core.svc);
        core.acceptor->open(endpoint.protocol());
        core.acceptor->set_option(tcp::acceptor::reuse_address(true));
#ifdef __linux__
        core.acceptor->bind(endpoint);
#else
        // Windows: SO_REUSEADDR allows multiple binds but only one gets connections.
        // We bind only on core 0; other cores just run idle for future expansion.
        if (i == 0) core.acceptor->bind(endpoint);
#endif
        core.acceptor->listen(tcp::socket::max_listen_connections);

        // Post initial accepts
        for (int a = 0; a < 16; ++a) {
            do_accept(i);
        }

#ifdef __linux__
        // Linux: one thread per io_service (per core)
        core.thread = std::thread([this, i] {
            asio::io_service::work work(*io_cores_[i].svc);
            io_cores_[i].svc->run();
        });
#else
        // Windows: multiple threads on single io_service
        for (size_t t = 0; t < n_io_threads; ++t) {
            io_threads_.emplace_back([this] {
                asio::io_service::work work(*io_cores_[0].svc);
                io_cores_[0].svc->run();
            });
        }
#endif
    }

    // Business thread count
    size_t n_biz = cfg.biz_threads > 0
        ? static_cast<size_t>(cfg.biz_threads)
        : std::max(1u, std::thread::hardware_concurrency() / 3);
    spdlog::info("Starting {} business threads", n_biz);

    biz_threads_.reserve(n_biz);
    for (size_t i = 0; i < n_biz; ++i) {
        biz_threads_.emplace_back(&server::business_thread_func, this);
    }
}
```

- [ ] **Step 2: Write server.cpp — do_accept**

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// Accept handler — creates session, assigns to a strand on the local io_core
// ─────────────────────────────────────────────────────────────────────────────
void server::do_accept(size_t core_idx) {
    auto& core = io_cores_[core_idx];
    if (!core.acceptor || !core.acceptor->is_open()) return;

    auto sock = std::make_shared<tcp::socket>(*core.svc);
    core.acceptor->async_accept(*sock,
        [this, &core, core_idx, sock](asio::error_code ec) {
            if (!ec) {
                asio::error_code ec_opt;
                sock->set_option(tcp::socket::receive_buffer_size(cfg_.socket_rcvbuf), ec_opt);
                sock->set_option(tcp::socket::send_buffer_size(cfg_.socket_sndbuf), ec_opt);
                sock->set_option(tcp::no_delay(cfg_.tcp_nodelay), ec_opt);

                LONG token = next_token_.fetch_add(1, std::memory_order_relaxed);
                size_t strand_idx = token % core.strands.size();

                auto sess = std::make_shared<session>(
                    std::move(*sock),
                    core.strands[strand_idx],
                    token,
                    [this](LONG t) { remove_session(t); },
                    ready_queue_,
                    cfg_);

                sessions_.insert(token, sess);
                sess->start();

                if (processor_) {
                    processor_->on_connect(token);
                }
            }
            if (!shutdown_.load(std::memory_order_acquire)) {
                do_accept(core_idx);
            }
        });
}
```

- [ ] **Step 3: Write server.cpp — business_thread_func**

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// Business thread — pops tokens from ready queue, drains ring buffers,
// dispatches to business_processor
// ─────────────────────────────────────────────────────────────────────────────
void server::business_thread_func() {
    while (!biz_shutdown_.load(std::memory_order_acquire)) {
        LONG token = ready_queue_.pop();
        if (token == 0) continue;  // shutdown signal

        auto sess = sessions_.find(token);
        if (!sess) continue;

        auto& ring = sess->ring();
        std::string msg;

        while (ring.dequeue(msg)) {
            if (sess->biz_closed()) break;

            try {
                if (processor_) {
                    auto response = processor_->on_message(token, msg);
                    if (!response.empty()) {
                        sess->post_write(response);
                    }
                }
            } catch (const std::exception& e) {
                spdlog::error("client {} business error: {}", token, e.what());
                break;
            }

            if (sess->biz_closed()) break;
        }

        // Check if session was closed (IO side detected disconnect)
        if (sess->biz_closed()) {
            if (processor_) {
                processor_->on_disconnect(token);
            }
            // close() already called by IO strand — no need to double-close
        }
    }
}
```

- [ ] **Step 4: Write server.cpp — send_message, remove_session, shutdown, ~server**

```cpp
// ─────────────────────────────────────────────────────────────────────────────
// send_message — thread-safe cross-core message sending
// ─────────────────────────────────────────────────────────────────────────────
void server::send_message(LONG token, const std::string& msg) {
    auto sess = sessions_.find(token);
    if (sess) {
        sess->post_write(msg);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_session — callback from session::close()
// ─────────────────────────────────────────────────────────────────────────────
void server::remove_session(LONG token) {
    sessions_.erase(token);
}

// ─────────────────────────────────────────────────────────────────────────────
// shutdown — graceful shutdown of all components
// ─────────────────────────────────────────────────────────────────────────────
void server::shutdown() {
    if (shutdown_.exchange(true)) return;

    // Stop acceptors
    for (auto& core : io_cores_) {
        if (core.acceptor) {
            asio::error_code ec;
            core.acceptor->close(ec);
        }
    }

    // Wake and stop business threads
    biz_shutdown_.store(true, std::memory_order_release);
    ready_queue_.shutdown();
    for (auto& t : biz_threads_) {
        if (t.joinable()) t.join();
    }

    // Stop IO services
    for (auto& core : io_cores_) {
        if (core.svc) {
            core.svc->stop();
        }
    }
    for (auto& core : io_cores_) {
        if (core.thread.joinable()) {
            core.thread.join();
        }
    }
    for (auto& t : io_threads_) {
        if (t.joinable()) t.join();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Destructor
// ─────────────────────────────────────────────────────────────────────────────
server::~server() {
    shutdown();
}
```

- [ ] **Step 5: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/server.cpp && git commit -m "feat: add server class implementation

- Multi-io_service architecture (per-core)
- SO_REUSEPORT on Linux, single-acceptor dispatch on Windows
- Work-stealing business thread pool via ready queue
- Sharded session map for low-contention lookups

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 10: `main.cpp` — 入口点

**Files:**
- Create: `tcpserver/main.cpp`

- [ ] **Step 1: Write main.cpp**

```cpp
#include "server.h"
#include "server_config.h"
#include <iostream>
#include <memory>
#include <spdlog\spdlog.h>

int main() {
    try {
        server_config cfg;
        cfg.listen_ip = "127.0.0.1";
        cfg.listen_port = 8888;
        cfg.log_level = spdlog::level::info;

        spdlog::set_level(cfg.log_level);

        auto processor = std::make_unique<echo_processor>();
        server s(cfg, std::move(processor));

        std::cout << "Server listening on " << cfg.listen_ip
                  << ":" << cfg.listen_port << "\n";
        std::cout << "Press 'Q' to quit\n";

        int ch;
        do {
            ch = std::toupper(std::getchar());
        } while (ch != 'Q');

        s.shutdown();
    } catch (const std::exception& e) {
        std::cerr << "Fatal: " << e.what() << "\n";
        return 1;
    }
    return 0;
}
```

- [ ] **Step 2: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/main.cpp && git commit -m "feat: add server entry point

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 11: 更新 vcxproj + 删除旧文件

**Files:**
- Modify: `tcpserver/tcpserver.vcxproj`
- Delete: `tcpserver/tcpserver.cpp` (replaced by new files)

- [ ] **Step 1: 更新 vcxproj — 替换 ClCompile 引用**

Read the current vcxproj to see the ItemGroup containing `tcpserver.cpp`.

Edit `tcpserver/tcpserver.vcxproj`, replacing:

```xml
  <ItemGroup>
    <ClCompile Include="tcpserver.cpp" />
  </ItemGroup>
```

with:

```xml
  <ItemGroup>
    <ClCompile Include="main.cpp" />
    <ClCompile Include="session.cpp" />
    <ClCompile Include="server.cpp" />
  </ItemGroup>
  <ItemGroup>
    <ClInclude Include="business_processor.h" />
    <ClInclude Include="ready_queue.h" />
    <ClInclude Include="server.h" />
    <ClInclude Include="server_config.h" />
    <ClInclude Include="session.h" />
    <ClInclude Include="sharded_map.h" />
    <ClInclude Include="spsc_ring.h" />
  </ItemGroup>
```

- [ ] **Step 2: 删除旧文件**

```bash
cd "D:\Work\Server\netlib" && git rm tcpserver/tcpserver.cpp
```

- [ ] **Step 3: 更新 CMakeLists.txt (如果使用)**

Read `tcpserver/CMakeLists.txt` and update source file list.

- [ ] **Step 4: Commit**

```bash
cd "D:\Work\Server\netlib" && git add tcpserver/tcpserver.vcxproj tcpserver/CMakeLists.txt && git commit -m "build: update vcxproj for new file structure

- Replace single tcpserver.cpp with main.cpp, session.cpp, server.cpp
- Add header files to project
- Remove old tcpserver.cpp

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

### Task 12: Build + 验证

**Files:**
- None (build verification only)

- [ ] **Step 1: Restore vcpkg dependencies (去除 boost)**

If `vcpkg.json` lists `boost`, remove the boost entry and run:

```bash
cd "D:\Work\Server\netlib" && vcpkg install
```

- [ ] **Step 2: Build Release x64**

```bash
cd "D:\Work\Server\netlib" && "D:\Soft\VS2022\MSBuild\Current\Bin\MSBuild.exe" tcpserver/tcpserver.vcxproj /p:Configuration=Release /p:Platform=x64 /v:minimal
```

Expected: Build succeeded with 0 errors.

- [ ] **Step 3: 运行 smoke test**

Start the server in one terminal:

```bash
start tcpserver/x64/Release/tcpserver.exe
```

Run the Go benchmark with a small load:

```bash
cd benchmark && go run main.go -c 10 -n 1000 -s 64 -addr 127.0.0.1:8888
```

Expected: All requests sent and responses received. No errors.

- [ ] **Step 4: Commit (if any fixes were needed)**

```bash
cd "D:\Work\Server\netlib" && git add -A && git commit -m "fix: build and smoke test fixes

Co-Authored-By: Claude Opus 4.8 <noreply@anthropic.com>"
```

---

## 实现顺序依赖

```
Task 1 (spsc_ring.h) ────┐
Task 2 (ready_queue.h) ──┤
Task 3 (server_config.h) ┤─── 无依赖，可并行
Task 4 (business_proc.h) ┤
Task 5 (sharded_map.h) ──┘
        │
        ▼
Task 6 (session.h) ── 依赖 1,2,3
        │
        ▼
Task 7 (session.cpp) ── 依赖 6
        │
        ▼
Task 8 (server.h) ── 依赖 3,5
        │
        ▼
Task 9 (server.cpp) ── 依赖 7,8
        │
        ▼
Task 10 (main.cpp) ── 依赖 8
        │
        ▼
Task 11 (vcxproj) ── 依赖 1-10
        │
        ▼
Task 12 (build) ── 依赖 11
```

---

## 关键设计决策记录

1. **spsc_ring 用 `std::vector` 而非 `std::array`** — 允许运行时配置容量，保持非模板化接口
2. **ready_queue 用 `std::mutex` + `std::condition_variable`** — push 频率低 (仅 ring buffer 空→非空)，锁竞争可忽略
3. **read buffer 在 session 中内嵌 `std::vector<char>`** — 初始实现简单，后续可改为 buffer pool
4. **Windows 单 acceptor 分发** — core 0 accept，轮询分发给其他 core (后续可加 `WSADuplicateSocket`)
5. **去除 boost 依赖** — `boost::lockfree::spsc_queue` 替换为自实现 `spsc_ring`
6. **session 持有 server_config 引用** — 避免重复拷贝，config 生命周期覆盖 server 运行期
