# TCP Server 高并发架构设计

**日期**: 2026-06-08
**版本**: v1.0
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

### 3.3 Write Path (零拷贝 scatter-gather)

```
post_write(data) [任意线程]:
  → 8字节头 + payload 封装为 string frame
  → strand_.post(λ: enqueue_write(frame))

enqueue_write(frame) [strand上下文]:
  → if write_queue_bytes + frame.size > max: drop + warn
  → write_msgs_.push_back(frame)
  → write_queue_bytes_ += frame.size
  → if was_empty: do_write()

do_write():
  → if count == 1: async_write(single_buffer)     // 快速路径
  → else: async_write(scatter-gather buffers[])    // 批量路径, writev/WSASend
```

**关键改进**: scatter-gather 替代 batch_buf_ 拷贝
```cpp
// 旧代码
batch_buf_.clear();
for (auto& msg : write_msgs_) batch_buf_ += msg;  // 拷贝全部
asio::async_write(socket, buffer(batch_buf_), ...);

// 新代码
std::vector<asio::const_buffer> buffers;
for (auto& msg : write_msgs_)
    buffers.push_back(asio::buffer(msg));  // 零拷贝
asio::async_write(socket, buffers, ...);   // → writev / WSASend(WSABUF[])
```

流控: `write_queue_max_bytes` 默认 256KB, `write_queue_max_count` 默认 1024, 均可配。

---

## 4. 消息总线设计

### 4.1 每 Session 双通道

| 方向 | 通道 | 特性 |
|------|------|------|
| IO → Biz (上行) | SPSC ring buffer | 自实现, lock-free, 批量出队 |
| Biz → IO (下行) | strand_.post() | 复用 asio 内置队列 |

### 4.2 SPSC Ring Buffer

```cpp
template<typename T, size_t Capacity>  // Capacity 必须是 2 的幂
class spsc_ring {
    static constexpr size_t mask = Capacity - 1;
    alignas(64) std::atomic<size_t> head_{0};  // IO strand 写
    alignas(64) std::atomic<size_t> tail_{0};  // 业务线程 写
    // cache line padding 防止 false sharing

    // enqueue: 单写者, 无需 CAS
    // dequeue: 单消费者, 无需 CAS
    // drain_available(): 批量查询可用消息数
};
```

配置: capacity 默认 1024 (2的幂)，总内存 ~64MB/50万连接。

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
        if (!token) { sem.acquire(); continue; }
        
        auto sess = session_map.find(token);
        auto& ring = sess->io_to_biz_ring();
        
        while (ring.dequeue(msg)) {
            auto rsp = processor->on_message(token, msg);
            if (!rsp.empty()) sess->post_write(std::move(rsp));
        }
    }
}
```

### 5.3 业务处理器接口

```cpp
class business_processor {
public:
    virtual ~business_processor() = default;
    virtual std::string on_message(LONG token, std::string_view data) = 0;
    virtual void on_connect(LONG token) {}
    virtual void on_disconnect(LONG token) {}
};
```

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
    int ring_buffer_capacity = 1024;
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

| 指标 | 当前 (bug修复后) | 新架构 |
|------|-----------------|--------|
| 并发连接 | ~5,000 | 500,000+ |
| 吞吐量 | ~30,000 msg/s | 2,000,000+ msg/s |
| P99 延迟 | ~ms | <100μs |
| 消息大小 | 64B ~ 4KB | 64B ~ 64KB |

---

## 10. 已确认的决策

- [x] 方案选择: C (分层消息总线)
- [x] IO:Business 核分离
- [x] 优先级: 吞吐量 > 延迟 > 连接数
- [x] 跨平台: Linux (SO_REUSEPORT) + Windows (WSADuplicateSocket)
- [x] 依赖: asio + spdlog, 去除 boost 依赖
- [x] 内存: buffer pool + 延迟分配, 不用 slab allocator
- [x] CMSG 通知: 仅 ring buffer 空→非空时通知
