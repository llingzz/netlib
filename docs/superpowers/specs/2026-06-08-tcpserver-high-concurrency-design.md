# TCP Server 高并发架构设计

**日期**: 2026-06-08
**版本**: v2.0 (2026-06-09 审查修订, 修复写路径瓶颈及 4 项正确性/性能缺陷)
**目标**: Tier 3 — 50万并发连接, 200万 msg/s 吞吐量

---

## 1. 概述

### 1.1 当前代码问题

| 类型 | 问题 | 位置 |
|------|------|------|
| Bug | `notify_biz_()` 被注释，业务线程永远阻塞 | L196 |
| 瓶颈 | 单 io_service 内部锁竞争 | 全局 |
| 瓶颈 | 全局 shared_mutex 在 send_message 热路径 | L424 |
| 瓶颈 | biz_slot 固定分片导致同槽 session 饿死 | L453-488 |
| 瓶颈 | batch_buf_ 每次重建拷贝全部数据 | L253 |
| 瓶颈 | remove_session O(N) 扫描 weak_ptr vector | L437-449 |
| 限制 | max_body=1024, SPSC capacity=256 硬编码 | L291 |

### 1.2 目标规模

| 指标 | 当前 (bug 修复后估算) | 目标 |
|------|----------------------|------|
| 并发连接数 | ~5,000 | 500,000+ |
| 吞吐量 | ~30,000 msg/s | 2,000,000+ msg/s |
| P99 延迟 | ~ms | <100μs |
| 消息大小 | 64B ~ 4KB | 64B ~ 64KB |

### 1.3 优先级

**吞吐量 > 延迟 > 连接数**

---

## 2. 架构总览

### 2.1 三层分离

```
                         ┌──────────────────────┐
                         │   Multi-Acceptor      │
                         │   (每 IO 核一个)       │
                         └──────────┬───────────┘
                                    │
           ┌────────────────────────┼────────────────────────┐
           │                   IO 层 (K 核)                   │
           │  ┌──────────┐   ┌──────────┐   ┌──────────┐     │
           │  │IO Core 0 │   │IO Core 1 │...│IO Core K-1│    │
           │  │io_svc    │   │io_svc    │   │io_svc    │     │
           │  │S strands │   │S strands │   │S strands │     │
           │  │acceptor  │   │acceptor  │   │acceptor  │     │
           │  └────┬─────┘   └────┬─────┘   └────┬─────┘     │
           └───────┼──────────────┼──────────────┼───────────┘
                   │              │               │
           ┌───────┴──────────────┴───────────────┴──────────┐
           │          消息总线 (per-session ring buffer)       │
           │  IO → Biz: SPSC ring buffer (上行)               │
           │  Biz → IO: strand_.post() (下行)                 │
           └───────────────┬──────────────────────────────────┘
                           │
           ┌───────────────┴──────────────────────────────────┐
           │              业务层 (J 核)                        │
           │  ┌──────────┐   ┌──────────┐   ┌──────────┐     │
           │  │Biz Core 0│   │Biz Core 1│...│Biz Core J-1│    │
           │  │pop token │   │pop token │   │pop token │     │
           │  │drain buf │   │drain buf │   │drain buf │     │
           │  │process   │   │process   │   │process   │     │
           │  └──────────┘   └──────────┘   └──────────┘     │
           └──────────────────────────────────────────────────┘
```

### 2.2 核心数据流

```
客户端 → NIC → IOCP → io_svc 唤醒 strand handler
  → parse_messages() 提取完整帧
  → push 到 session 的 IO→Biz ring buffer
  → if (ring buffer 之前为空) push token 到全局就绪队列

业务线程:
  → pop 就绪队列拿到 token → 定位 session
  → drain ring buffer (批量取出所有消息)
  → 逐条调用 on_business_message(token, msg)
  → 如需回复: sess->post_write(rsp) → strand.post → enqueue_write → do_write
```

### 2.3 关键设计决策

| 决策 | 选择 | 理由 |
|------|------|------|
| IO:业务 核隔离 | 各自独立线程+io_service | 避免业务慢处理阻塞 IO |
| 消息通道 | SPSC ring buffer per session | 锁-free, cache-friendly |
| 通知机制 | 全局 MPSC 就绪队列 + sem | 避免 per-session eventfd 过多 |
| 响应路径 | strand_.post() | 复用 asio 机制, 无需额外队列 |
| Acceptor | 每 IO 核一个 | 消除单 acceptor 瓶颈 |
| Session 表 | 256 分片 rwlock+map | 锁竞争降低 256 倍 |

---

## 3. IO 层设计

### 3.1 多 io_service

- K 个 `asio::io_service`，每个跑 1 个线程（默认 `hardware_concurrency * 2/3`）
- 可选 CPU 绑核 (`SetThreadAffinityMask` / `pthread_setaffinity`)
- Linux: `SO_REUSEPORT`，每个 io_service 独立 bind+listen
- Windows: 主 acceptor accept 后 `WSADuplicateSocket` 分发给目标 io_service
- 每 io_service 内 S 个 strand（默认 64），session 按 `token % S` 分配

### 3.2 Read Path

继承当前设计的优化方案，微调：

```
do_read():
  buffer = buffer_pool.acquire(read_buf_size)
  async_read(socket, buffer[data_len_ .. read_buf_size])
    → data_len_ += bytes_read
    → parse_messages():
        while pos + 8 <= data_len_:
          from_chars → body_len
          校验 body_len (1..max_body, 可配)
          if incomplete: break
          push string_view to ring buffer
          pos += 8 + body_len
        memmove residual to buffer[0]
        data_len_ -= pos
    → if !read_eof_: do_read()
    → else: try_finish()
```

改进点：
- `max_body` 可配（默认 65536）
- `read_buf_size` 可配（默认 64KB）
- buffer 从对象池分配，空闲连接不占用 64KB

### 3.3 Write Path (Biz→IO SPSC 写环 + Scatter-Gather)

#### v2.0 重大改进：与读路径对称的无锁写通道

v1.0 设计中写路径每消息调用 `strand_.post()`，在 200万 msg/s 下每秒 200万次 strand 内部 mutex + lambda 堆分配，成为致命瓶颈。v2.0 采用与读路径对称的「仅空→非空时通知」机制。

#### 架构

```
session 内新增: SPSC write_ring (Biz→IO 方向)

post_write(data) [业务线程]:
  → 8字节头 + payload 封装为 string frame
  → was_empty = write_ring_.enqueue(frame) 返回值  ← lock-free, 无 strand 调度
  → if was_empty: strand_.post(λ: do_write_kick()) ← 仅空→非空时触发!

do_write_kick() [strand上下文]:
  → 从 write_ring_ 批量 drain 到 write_msgs_
  → do_write() scatter-gather 批量发送
  → 完成回调中再次 drain + do_write (写链自持)
```

#### 关键代码

```cpp
// session::post_write — 业务线程调用 (热路径, 无锁)
void session::post_write(std::string data) {
    if (data.size() > cfg_.max_payload) return;
    char head[16];
    snprintf(head, sizeof(head), "%08d", (int)data.size());
    std::string frame = std::string(head, 8) + data;

    bool was_empty;
    if (!write_ring_.enqueue(std::move(frame), was_empty)) {
        spdlog::warn("client {} write ring full, dropping", tokenid_);
        return;  // 背压: 丢弃
    }
    // ★ 仅当写环从空变非空时，才触发一次 strand 调度
    if (was_empty) {
        auto self = shared_from_this();
        strand_.post([this, self] { do_write_kick(); });
    }
}

// do_write_kick — strand 上下文, 批量 drain → scatter-gather 发送
void session::do_write_kick() {
    // 从无锁写环批量取出所有待发送帧
    write_msgs_.clear();
    write_queue_bytes_ = 0;
    std::string frame;
    while (write_ring_.dequeue(frame)) {
        write_queue_bytes_ += frame.size();
        write_msgs_.push_back(std::move(frame));
    }
    if (!write_msgs_.empty()) {
        do_write();  // scatter-gather 批量发送
    }
}

// do_write — strand 上下文, scatter-gather async_write
void session::do_write() {
    if (write_msgs_.size() == 1) {
        // 快速路径: 单帧直接发送
        async_write(socket_, asio::buffer(write_msgs_.front()),
            strand_.wrap([this, self = shared_from_this()](auto ec, auto) {
                if (ec) { on_error(ec); return; }
                write_queue_bytes_ -= write_msgs_.front().size();
                write_msgs_.pop_front();
                do_write_kick();  // drain 写环中可能新增的帧
            }));
    } else {
        // 批量路径: scatter-gather 零拷贝 (writev / WSASend)
        scatter_bufs_.clear();
        for (auto& msg : write_msgs_) scatter_bufs_.push_back(asio::buffer(msg));
        size_t count = write_msgs_.size();
        async_write(socket_, scatter_bufs_,
            strand_.wrap([this, self = shared_from_this(), count](auto ec, auto) {
                if (ec) { on_error(ec); return; }
                for (size_t i = 0; i < count; ++i) {
                    write_queue_bytes_ -= write_msgs_.front().size();
                    write_msgs_.pop_front();
                }
                do_write_kick();  // drain 写环中可能新增的帧
            }));
    }
}
```

#### 设计要点

| 特性 | 说明 |
|------|------|
| 热路径无锁 | `write_ring_.enqueue()` 仅两个原子操作 + memcpy，无 CAS |
| strand 调度降频 | 从「每消息一次」降为「每批消息一次」(10-100x 降低) |
| 写链自持 | 完成回调 → `do_write_kick()` → drain 写环 → `do_write()` → 循环 |
| 流控 | `write_ring_` 满时丢弃, 写环容量默认 1024 (可配) |
| 单生产者保证 | 一个 session 同一时刻只被一个业务线程处理, SPSC 无需 CAS |
| 外部调用兼容 | `send_message()` 也走 `post_write()`, 同一 session 的外部并发由上层保证 |

#### 对比 v1.0

| 方面 | v1.0 | v2.0 |
|------|------|------|
| 下行通道 | `strand_.post()` (asio 内部 mutex+队列) | SPSC write_ring (lock-free) |
| strand 调度频率 | 每消息 1 次 | 每批消息 1 次 |
| lambda 分配 | 每消息 1 次 (`[self, frame]`) | 每批消息 1 次 |
| 内存顺序 | asio 内部保证 | acquire/release (自定义) |
| 设计对称性 | 读写路径不对称 | 读写均 SPSC + empty→non-empty 通知 |

---

## 4. 消息总线设计

### 4.1 每 Session 双通道

| 方向 | 通道 | 特性 |
|------|------|------|
| IO → Biz (上行) | SPSC ring buffer (io_to_biz_ring_) | 自实现, lock-free, 批量出队 |
| Biz → IO (下行) | SPSC ring buffer (write_ring_) | 自实现, lock-free, **strand_.post() 仅空→非空时触发** |

#### 设计对称性 (v2.0)

```
上行 (IO→Biz):                         下行 (Biz→IO):
  ring_.enqueue(msg)                     write_ring_.enqueue(frame)
  if (was_empty)                         if (was_empty)
    ready_queue_.push(token)               strand_.post(do_write_kick)

  仅空→非空时通知 ✓                      仅空→非空时通知 ✓
```

两条通道采用完全相同的通知策略：仅在 ring buffer 从空变为非空时触发一次 wakeup 或 strand 调度。其余时间，消费者（业务线程/IO strand）处于自驱动状态：drain → 发送 → 回调中再次 drain。

### 4.2 SPSC Ring Buffer

```cpp
template<typename T, size_t Capacity>  // Capacity 必须是 2 的幂
class spsc_ring {
    static constexpr size_t mask = Capacity - 1;
    alignas(64) std::atomic<size_t> head_{0};  // 生产者写
    alignas(64) std::atomic<size_t> tail_{0};  // 消费者写
    alignas(64) std::vector<T> buf_;           // cache line 隔离
    // enqueue / dequeue: 均为单写者, 无需 CAS
};
```

#### 关键 API

```cpp
// 生产者: 入队一个元素。
// was_empty: 返回入队前 ring 是否为空 (用于通知决策)。
// 返回值: true=成功, false=满。
// ★ was_empty 和入队操作是原子的 (相对于消费者)。
bool enqueue(T item, bool& was_empty) {
    size_t h = head_.load(std::memory_order_relaxed);
    size_t t = tail_.load(std::memory_order_acquire);
    if (h - t >= buf_.size()) return false;
    was_empty = (h == t);                    // ★ 与 load(t) 同一快照
    buf_[h & mask] = std::move(item);
    head_.store(h + 1, std::memory_order_release);
    return true;
}
```

#### 正确性：Lost Wakeup 防护

v1.0 设计中 `was_empty = ring_.empty()` 和 `enqueue()` 分两步调用，存在 TOCTOU 竞态：
1. Producer 检查 empty() → false (ring 有数据)
2. Consumer 清空 ring → 准备阻塞
3. Producer enqueue() → was_empty 仍为 false → 不推送 token
4. **Lost wakeup：ring 中有数据，但无人通知 consumer**

v2.0 修复：`enqueue()` 在持有 head_/tail_ 快照的同一临界区内判定 `was_empty`。配合消费者**drain 后 acquire 重检** (见 §5.2)，彻底消除竞态。

#### 内存开销注意 (Tier 3)

`std::vector<std::string>` 每槽一个 string 对象 (~32B on MSVC)。50万连接 × 1024 槽 = 5.12 亿 string 对象 ≈ **16 GB** (仅 ring buffer，不含实际消息数据)。Tier 3 实现需替换为更紧凑的存储：

| 方案 | 每槽开销 | 50万连接总开销 | 复杂度 |
|------|---------|---------------|--------|
| `std::vector<std::string>` (当前) | ~32B + 数据 | ~16 GB | 低 |
| 预分配 flat buffer + offset/length | ~8B + 数据 | ~4 GB | 中 |
| `std::string` with custom pool allocator | ~32B + pool | ~16 GB + pool | 中 |
| 延迟分配 (仅使用时分配槽) | ~8B (指针) + 数据 | ~4 GB (10% 活跃) | 高 |

**v2.0 策略**：默认使用 `std::vector<std::string>` (实现简单)，通过 `ring_buffer_capacity` 配置按需降低 (如 256)。Tier 3 部署时启用 flat buffer 模式 (编译期 `#ifdef SPSC_RING_FLAT_BUFFER`)。

配置: capacity 默认 1024 (2的幂)。

### 4.3 通知机制

```
全局 MPSC 就绪队列 (bounded, 默认 65536 槽):
  入队: IO strand push 消息后, if ring buffer 之前为空 → push token + notify_one
  出队: 业务线程 pop token → process_one_session(token)
  
Token 不重复入队保证:
  - ring buffer 从空变非空 → push token
  - ring buffer 本来就非空 → session 已在队列/处理中, 不重复 push
```

### 4.4 关闭信号

```
IO strand 检测断开:
  1. push "关闭哨兵" 到 ring buffer
  2. if ring buffer 之前为空 → push token 到就绪队列
  3. 业务线程 drain → 遇到哨兵 → 处理残余 → on_disconnect() → final_close()
```

---

## 5. 业务层设计

### 5.1 线程模型

- J 个业务线程（默认 `hardware_concurrency * 1/3`）
- 全局 MPSC 就绪队列 → 自然负载均衡
- 每个 session 同一时刻只被一个业务线程处理（由 token 唯一性保证）

### 5.2 处理循环

```cpp
void business_thread_loop() {
    while (!shutdown) {
        LONG token = ready_queue.pop();
        if (token == 0) continue;  // shutdown signal

        auto sess = sessions_.find(token);
        if (!sess || sess->biz_closed()) continue;

        auto& io_ring = sess->io_to_biz_ring();

        // ★ 批量 drain → 批量处理 → 批量回复
        std::vector<std::string> batch;
        batch.reserve(64);
        std::string msg;

        // 第一轮 drain
        while (io_ring.dequeue(msg)) {
            if (sess->biz_closed()) goto cleanup;
            batch.push_back(std::move(msg));
        }

        // ★ Lost wakeup 防护: acquire 重检
        // 生产者在 batch drain 之后可能入队了新消息
        // dequeue() 内部 load(head_, acquire) 可见生产者的 release store
        while (io_ring.dequeue(msg)) {
            if (sess->biz_closed()) goto cleanup;
            batch.push_back(std::move(msg));
        }

        if (!batch.empty()) {
            processor->on_message_batch(token, batch,
                [&sess](std::string rsp) {
                    sess->post_write(std::move(rsp));  // → SPSC write_ring
                });
            batch.clear();
        }

    cleanup:
        if (sess->biz_closed()) {
            // drain 残余消息 (关闭哨兵可能已在 ring 中)
            while (io_ring.dequeue(msg)) {
                processor->on_message(token, msg);
            }
            processor->on_disconnect(token);
        }
    }
}
```

#### 关键改进 (v2.0)

| 改进 | 说明 |
|------|------|
| **Lost wakeup 防护** | drain 循环结束后再次 `dequeue()` — 其内部 `load(head_, acquire)` 可见生产者在 drain 期间入队的消息。若取出数据，继续 drain 而不阻塞 |
| **批量处理** | 一次 drain 一批消息，批量传给 processor，减少虚函数调用次数 |
| **批量回复** | processor 通过 reply callback 回复，内部直写 SPSC write_ring |
| **关闭处理** | `biz_closed_` 检测后 drain 残余 → 调用 `on_disconnect`，确保不丢消息 |

### 5.3 业务处理器接口

```cpp
class business_processor {
public:
    virtual ~business_processor() = default;

    // 批量处理消息。默认实现逐条调用 on_message。
    // reply(std::string) 用于发送响应，空字符串表示不回复。
    virtual void on_message_batch(LONG token,
        const std::vector<std::string>& messages,
        const std::function<void(std::string)>& reply)
    {
        for (auto& msg : messages) {
            auto rsp = on_message(token, msg);
            if (!rsp.empty()) reply(std::move(rsp));
        }
    }

    // 单条处理 (向后兼容, 可 override)
    virtual std::string on_message(LONG token, std::string_view data) {
        return {};  // 默认不回复
    }

    virtual void on_connect(LONG token) {}
    virtual void on_disconnect(LONG token) {}
};
```

#### 对比 v1.0

| 方面 | v1.0 | v2.0 |
|------|------|------|
| 返回值 | `std::string` (空响应也分配) | `void` + reply callback (不回复则零分配) |
| 批量 | 无 (逐条 on_message) | `on_message_batch` + reply callback |
| 虚函数调用 | 每条消息 1 次 | 每批消息 1 次 (batch mode) |

### 5.4 对比旧设计

| 方面 | 旧设计 | 新设计 |
|------|--------|--------|
| 分片方式 | token % M 固定槽位 | 全局就绪队列 work-stealing |
| 公平性 | 一个 session 可能饿死同槽其他 | 每次一个 session, 自然公平 |
| 锁 | slot.sessions_mtx | 就绪队列 lock-free |
| 唤醒 | 每条消息通知(被注释了) | 仅 ring buffer 从空→非空时通知 |

---

## 6. Session 管理

### 6.1 Sharded Session Map

```
256 分片，每分片独立 shared_mutex + unordered_map<LONG, shared_ptr<session>>
shard_idx = hash(token) & 0xFF

find:    shared_lock → find
insert:  unique_lock → map[token] = sess
erase:   unique_lock → map.erase(token)
```

### 6.2 send_message (跨核)

```cpp
void send_message(LONG token, const std::string& msg) {
    size_t shard = token & 0xFF;
    std::shared_ptr<session> sess;
    {
        std::shared_lock lock(shards_[shard].mtx);
        auto it = shards_[shard].map.find(token);
        if (it == shards_[shard].map.end()) return;
        sess = it->second;  // shared_ptr 引用计数保护
    }
    if (sess) sess->post_write(msg);  // strand_.post() 自动处理跨核同步
}
```

### 6.3 Session 生命周期

```
ACTIVE ──FIN/EOF──▶ DRAINING (等write排空) ──▶ CLOSING
  │                                              ▲
  └──RST/error──────────────────────────────────┘
  
CLOSING:
  - closed_.exchange(true) 幂等保护
  - shutdown socket
  - push 关闭哨兵到 ring buffer
  - cb_remove_() 从 sharded map 移除
```

### 6.4 内存策略

| 资源 | 策略 | 空闲连接占用 |
|------|------|-------------|
| session 对象 | shared_ptr 管理 | ~200B |
| read buffer | 对象池, 仅在读时分配 | 0 |
| ring buffer 存储 | 延迟分配(首次push时) | 0 |
| write_msgs_ | deque (通常很少) | ~0 |

---

## 7. 配置参数

### 7.1 完整配置

```cpp
struct server_config {
    std::string listen_ip = "0.0.0.0";
    int listen_port = 8888;
    
    int io_threads = 0;          // 0 = auto
    int strands_per_io = 64;
    int socket_rcvbuf = 65536;
    int socket_sndbuf = 65536;
    bool tcp_nodelay = true;
    
    int biz_threads = 0;         // 0 = auto
    int ring_buffer_capacity = 1024;    // io_to_biz_ring_ 容量
    int write_ring_capacity = 1024;     // write_ring_ 容量 (Biz→IO)
    int ready_queue_capacity = 65536;
    int max_payload = 65536;
    
    int write_queue_max_bytes = 262144;
    int write_queue_max_count = 1024;
    int read_buf_size = 65536;
    
    int session_map_shards = 256;
    spdlog::level::level_enum log_level = spdlog::level::info;
    bool cpu_affinity = false;
};
```

### 7.2 场景推荐值

| 参数 | Tier 1 | Tier 2 | Tier 3 |
|------|--------|--------|--------|
| io_threads | 4 | auto | auto+cpu_affinity |
| biz_threads | 2 | auto | auto+cpu_affinity |
| ring_buf_capacity | 256 | 1024 | 4096 |
| write_ring_capacity | 256 | 1024 | 4096 |
| write_q_max_bytes | 64KB | 256KB | 1MB |
| read_buf_size | 64KB | 64KB | 256KB |
| session_map_shards | 64 | 256 | 1024 |

---

## 8. 文件结构

```
tcpserver/
├── main.cpp                  # 入口, 命令行解析
├── server_config.h           # 配置结构体
├── server.h / server.cpp     # server 类
├── session.h / session.cpp   # session 类
├── spsc_ring.h               # SPSC ring buffer (header-only)
├── ready_queue.h             # MPSC 就绪队列 (header-only)
├── business_processor.h      # 业务处理器接口
├── sharded_map.h             # 分片 map (header-only)
├── buffer_pool.h             # buffer 对象池 (header-only)
└── tcpserver.vcxproj
```

---

## 9. 预期性能

| 指标 | v1.0 估算 | v2.0 估算 | 备注 |
|------|----------|----------|------|
| 并发连接 | 500,000+ | 500,000+ | 取决于内存配置 |
| 吞吐量 (msg/s) | 2,000,000+ | 3,000,000+ | v2.0 消除 strand_.post() 瓶颈 |
| P99 延迟 | <100μs | <50μs | v2.0 写路径降延迟 |
| 消息大小 | 64B ~ 64KB | 64B ~ 64KB | — |
| strand_.post()/msg | 1.0 | ~0.05 (20x 降低) | 仅空→非空时触发 |
| 写路径锁操作/msg | 1 (strand内部mutex) | 0 (SPSC) | v2.0 无锁热路径 |

---

## 10. 已确认的决策

- [x] 方案选择: C (分层消息总线)
- [x] IO:Business 核分离
- [x] 优先级: 吞吐量 > 延迟 > 连接数
- [x] 跨平台: Linux (SO_REUSEPORT) + Windows (WSADuplicateSocket)
- [x] 依赖: asio + spdlog, 去除 boost 依赖
- [x] 内存: buffer pool + 延迟分配, 不用 slab allocator
- [x] 通知策略: 仅 ring buffer 空→非空时通知 (读写路径对称)
- [x] **v2.0** 写路径: SPSC write_ring + strand_.post() 仅空→非空 (替代 v1.0 每消息 strand_.post())
- [x] **v2.0** 业务接口: `on_message_batch` + reply callback (替代 v1.0 `string on_message`)
- [x] **v2.0** Lost wakeup 防护: `enqueue()` 原子返回 `was_empty` + consumer acquire 重检

---

## 11. 实现路线图

| 阶段 | 内容 | 目标 |
|------|------|------|
| Phase 1 | v2.0 核心架构实现 (spsc_ring, write_ring, 批量处理) | Tier 1 (1万连接, 10万 msg/s) |
| Phase 2 | 多 io_service + SO_REUSEPORT + 分片 map | Tier 2 (10万连接, 50万 msg/s) |
| Phase 3 | Buffer pool + flat buffer 存储 + CPU 绑核 | Tier 3 (50万连接, 200万+ msg/s) |

---

## 12. 设计审查：已知限制与待解决问题 (2026-06-09)

### 12.1 已修复 (v2.0)

| # | 问题 | 严重度 | 修复 |
|---|------|--------|------|
| W1 | 写路径每消息 `strand_.post()` — 200万 msg/s 时每秒 200 万次 strand mutex + lambda 分配 | **致命** | §3.3 SPSC write_ring + empty→non-empty 通知 |
| W2 | Lost wakeup: `ring_.empty()` 和 `enqueue()` 非原子, consumer drain 后无 acquire 重检 | **致命** | §4.2 enqueue 原子返回 was_empty; §5.2 consumer 重检 |
| W3 | 关闭消息丢失: `biz_closed_` 设置后 ring 中残余消息被丢弃 | **严重** | §5.2 cleanup 段 drain 残余后调用 on_disconnect |
| W4 | `on_message` 返回 `std::string`, 空响应也分配内存 | **中等** | §5.3 reply callback, 不回复则零分配 |
| W5 | 无批处理接口, 每条消息一次虚函数调用 | **中等** | §5.3 `on_message_batch` 批量接口 |

### 12.2 Tier 3 部署前必须解决

| # | 问题 | 严重度 | 影响 | 建议方案 |
|---|------|--------|------|---------|
| R1 | `spsc_ring` 用 `std::vector<std::string>` — 50万连接 × 1024 槽 ≈ 16 GB (仅 ring 对象) | **严重** | Tier 3 内存不可行 | Phase 3: flat buffer 模式 (自定义 allocator + offset/length 对) |
| R2 | Read buffer pool 未实现 — plan 退化为 `std::vector<char>` 嵌入 session: 50万 × 256KB = 128 GB | **严重** | Tier 3 内存不可行 | Phase 3: 对象池, 仅活跃连接持有 buffer |
| R3 | Windows 单 io_service — IOCP 虽支持多线程, 但单 accept 分发为瓶颈 | **中等** | Windows Tier 3 吞吐 | WSADuplicateSocket 分发到多 io_service |
| R4 | 写 SPSC 队列单生产者假设 — 外部 `send_message()` 与业务线程可能并发写同一 session | **中等** | 外部 API 并发写时数据竞争 | 方案A: `send_message` 通过 strand_.post 间接写; 方案B: 改用 MPSC 队列 |

### 12.3 已知设计权衡

| # | 问题 | 权衡 | 缓解措施 |
|---|------|------|---------|
| T1 | 无业务背压 — ring buffer 满即丢弃 | 简单性 vs 可靠投递 | `ring_buffer_capacity` 可配 (增大延迟换可靠性) |
| T2 | 就绪队列有界 (65536) — 满时 token 丢失 | 背压 vs 无界内存 | `ready_queue_capacity` 可配; token 丢失时该 session 由后续消息唤醒 |
| T3 | 业务线程 `session_map.find()` 含 shared_lock — 每 token pop 一次 | 锁开销 vs 架构简洁 | 256 分片使竞争降低 256×; 后续可缓存 session ptr |
| T4 | `std::string` 消息拷贝 — ring buffer 存储完整 string | 实现简单 vs 零拷贝 | frame 本身需封装 header, 拷贝不可免; Phase 3 可用 flat buffer 减少分配次数 |
| T5 | 关闭序列 session 不显式 close — 依赖 shared_ptr 析构 | 简化生命周期 vs 显式控制 | `shutdown()` 停止 io_service 后 session 析构; 待写数据可能丢失 (可接受) |

### 12.4 后续优化 (Phase 3+)

| # | 优化点 | 预期收益 |
|---|--------|---------|
| O1 | CPU 绑核 + 中断亲和 — IO 线程绑独立核, 业务线程绑剩余核 | 缓存局部性, 降低 tail latency |
| O2 | `io_service` per core (Windows WSADuplicateSocket) | Windows 平台吞吐接近 Linux |
| O3 | Frame 预分配池 — 每连接预分配 N 个 frame buffer, 循环使用 | 减少 malloc/free |
| O4 | 内置 metrics — 消息计数, 队列深度, 连接数, 延迟直方图 | 可观测性 |
| O5 | Connection rate limiter — token bucket | 防止连接风暴 |
| O6 | Ring buffer 自动扩容 — 检测到持续接近满时渐进扩容 | 避免消息丢弃 |
