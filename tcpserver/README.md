# tcpserver — 高性能 TCP 服务框架

基于 asio 的高并发 TCP 服务端框架，核心引擎只做 TCP 字节流收发，
协议编解码（framing）由 Framework 层通过可插拔 codec 提供。

## 分层架构

```
┌──────────────────────────────────────────────────────────────┐
│                   examples/ (示例 + 统一入口)                  │
├──────────────────────────────────────────────────────────────┤
│  tcp_service  │  middleware  │  dispatcher  │  codec         │  ← Framework
├──────────────────────────────────────────────────────────────┤
│  application (缓冲→拆帧→middleware链→on_message, 生命周期)     │
├──────────────────────────────────────────────────────────────┤
│  server  │  session  │  buffer_pool  │  shared_frame_pool   │  ← Core
├──────────────────────────────────────────────────────────────┤
│                    asio  │  spdlog                            │
└──────────────────────────────────────────────────────────────┘
```

### Core 层 — 网络引擎（零协议感知）

只管从 socket 读字节、写字节。不知"消息边界"为何物。

| 组件 | 职责 |
|------|------|
| `server` | 多 io_context accept、io/biz 线程池、全局 metrics、监控定时器 |
| `session` | 单连接读写：缓冲 → 投递原始字节块 / 写原始字节 → 异步写，零拷贝 buffer 移交 |
| `buffer_pool` | 64KB 固定大小读缓冲池，lock-free Treiber stack |
| `shared_frame_pool` | 8 档 (512B~64KB) 帧缓冲池，ABA-safe tagged pointer，lock-free |
| `work_queue` | io → biz 线程的原始数据块队列 |
| `config` | server_config 配置结构体 |

关键设计：
- **零协议**: session 不做拆帧/组帧，读写路径不包含任何协议逻辑
- **Inline 模式** (`biz_threads_enabled=false`): io 线程直接回调，零跨线程，适合 echo/转发
- **Separate 模式** (`biz_threads_enabled=true`): io/biz 线程分离，适合慢业务
- **零拷贝读路径**: `read_buf` 从 buffer_pool 获取，处理完后归还
- **背压保护**: 写队列超限自动丢弃帧，防止 OOM

### Framework 层 — 业务框架

在 Core 层原始字节流之上，提供协议编解码、消息路由、中间件等业务抽象。

| 组件 | 职责 |
|------|------|
| `tcp_service` | 业务服务基类 — 生命周期 + 消息处理 + 注入 codec |
| `codec` | **协议编解码器** — 定义消息边界（拆帧/组帧），可插拔 |
| `middleware` | 中间件基类 — 连接/消息横切关注点 |
| `message_dispatcher` | 消息路由器 — 按首字节 type_id 分发 |
| `application` | 应用脚手架 — per-session 缓冲、codec 拆帧、middleware 链、信号处理、启动/关闭 |

### Codec — 可插拔协议

`tcp_service` 默认使用 `length_prefixed_codec`，子类可在构造函数中替换 `codec_`。

| codec | 帧格式 | 适用场景 |
|-------|--------|----------|
| `length_prefixed_codec` (默认) | `[2B big-endian length][payload]` | 自定义二进制协议 |
| `raw_codec` | 无，字节透传 | HTTP / 行协议 / 自描述协议 |

**默认 codec 数据流**（echo/chat/dispatcher — 无须改动）：

```
socket → session 投递原始字节 → application 缓冲
  → length_prefixed_codec.decode 拆帧
  → middleware.before → service.on_message(完整消息) → middleware.after
  → reply(payload) → codec.encode(帧) → socket
```

**raw codec 数据流**（HTTP — 浏览器可直接访问）：

```
socket → session 投递原始字节 → application 缓冲 → raw_codec.decode 透传
  → service.on_message(原始TCP数据块) → HTTP parser 自行处理粘包分包
  → reply_raw(HTTP响应字节) → socket
```

编写 raw codec 服务只需在构造函数中替换 `codec_`：

```cpp
class http_service : public tcp_service {
public:
    http_service() {
        codec_ = std::make_unique<raw_codec>();  // 关掉 2B 长度头
    }
    void on_message(session* sess, std::string_view raw_data) override {
        // raw_data 就是 TCP 收到的原始字节
        // 自己解析 HTTP 请求、构建响应
        reply_raw(sess, http_response_string);
    }
};
```

### Middleware — 内置中间件

| 中间件 | 级别 | 功能 |
|--------|------|------|
| `connection_limiter` | on_connect | 最大连接数限制，超限拒绝新连接 |
| `rate_limiter` | before | Token bucket 单连接限速 |
| `access_log` | on_connect/on_disconnect | 统一打印连接/断开日志（含对端地址） |

## 线程模型

| 配置 | on_message 执行线程 | 适用场景 |
|------|---------------------|----------|
| `biz_threads_enabled = true` | biz 线程池（默认同 session 串行保序） | 有状态业务 |
| `biz_threads_enabled = false` | io 线程 | 纯 echo/转发，零跨线程开销 |

## 构建

### 依赖

- C++17
- [asio](https://think-async.com/Asio/) (header-only)
- [spdlog](https://github.com/gabime/spdlog)
- vcpkg (Windows) 或 apt (Linux)

### CMake

```bash
cd tcpserver
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=Release
cmake --build .
```

### Visual Studio

打开 `netlib.sln`，直接编译 tcpserver 项目。vcpkg 会自动安装依赖。

## 运行

```bash
tcpserver echo        # 启动 Echo 服务 (端口 8899)
tcpserver chat        # 启动聊天室服务
tcpserver dispatcher  # 启动多消息类型演示
tcpserver http        # 启动 HTTP 服务 (浏览器打开 http://localhost:8899)
```

### 命令行参数

| 参数 | 说明 | 适用模式 |
|------|------|----------|
| `--port N` | 监听端口 (默认 8899) | 全部 |
| `--inline` | 使用 inline 模式 | echo, http |

## 快速开始

### 三步写一个 TCP 服务（默认 2B 长度头协议）

**1. 继承 `tcp_service`，实现 `on_message`：**

```cpp
#include "framework/service.h"

struct my_service : tcp_service {
    void on_message(session* sess, std::string_view msg) override {
        reply(sess, "OK");  // 自动走 length_prefixed_codec 组帧
    }
};
```

**2. 创建 `application` 并启动：**

```cpp
#include "framework/application.h"

int main() {
    server_config cfg;
    cfg.listen_port = 9000;

    application app(cfg, std::make_unique<my_service>());
    return app.run();  // 阻塞，SIGINT/SIGTERM 优雅退出
}
```

**3. 编译运行：**

```bash
printf '\x00\x02OK' | nc localhost 9000
# → 收到 0x0002 + "OK" 即长度前缀帧
```

### HTTP 服务（raw codec，浏览器直接访问）

```cpp
#include "framework/service.h"
#include "framework/codec/raw_codec.h"

struct my_http : tcp_service {
    my_http() { codec_ = std::make_unique<raw_codec>(); }

    void on_message(session* sess, std::string_view raw) override {
        // raw = TCP 原始字节，自行解析 HTTP
        reply_raw(sess, "HTTP/1.1 200 OK\r\nContent-Length: 5\r\n\r\nHello");
    }
};
```

### 使用中间件

```cpp
application app(cfg, std::make_unique<my_service>());

app.use(std::make_unique<connection_limiter>(1000));  // 最多 1000 连接
app.use(std::make_unique<access_log>());              // 统一日志

app.run();
```

中间件执行顺序（按 `use()` 注册顺序）：

```
新连接 → mw[0].on_connect → mw[1].on_connect → ... → service.on_connect
                        (任一返回 false 则拒绝连接)

新消息 → mw[0].before → mw[1].before → service.on_message
                                              ↓
       ← mw[1].after  ← mw[0].after
  (任一 before 返回 false 则丢弃消息)
```

### 使用消息路由器

多消息类型按首字节 `type_id` 自动分发：

```cpp
#include "framework/dispatcher.h"

struct my_service : tcp_service {
    message_dispatcher dispatch_;

    my_service() {
        dispatch_
            .on(0x01, [this](session* s, auto m) { handle_login(s, m); })
            .on(0x02, [this](session* s, auto m) { handle_chat(s, m);  })
            .fallback([](session* s, auto m) { spdlog::warn("unknown type"); });
    }

    void on_message(session* sess, std::string_view msg) override {
        dispatch_.dispatch(sess, msg);  // 读 msg[0] 查表分发
    }
};
```

### 自定义中间件

```cpp
#include "framework/middleware.h"

class ip_blacklist : public middleware {
    bool on_connect(session* sess) override {
        if (is_blocked(sess->remote_addr()))
            return false;  // 拒绝连接
        return true;
    }
};
```

### 自定义 Codec

实现 `codec` 接口的三个方法：

```cpp
#include "framework/codec/codec.h"

class line_codec : public codec {
    std::vector<std::pair<uint16_t, uint16_t>>
        decode(const char* data, size_t len, size_t& consumed) override {
        // 按 '\n' 拆行
        // ...
    }
    std::string encode(std::string_view payload) override {
        return std::string(payload) + "\n";
    }
    size_t header_overhead() const override { return 0; }
};
```

## tcp_service 完整生命周期

```
on_start(server&)     — server 启动后
on_connect(session*)  — 新连接建立（middleware 全部通过后）
on_message(session*, string_view) — 收到消息（codec 拆帧后，纯虚）
on_disconnect(session*) — 连接断开
on_stop(server&)      — server 停止前
```

## 目录结构

```
tcpserver/
├── core/                         # 网络引擎（零协议）
│   ├── server.h/cpp              # 多 io_context acceptor、线程池、metrics
│   ├── session.h/cpp             # 单连接读写、原始字节投递、零拷贝 buffer 移交
│   ├── buffer_pool.h             # 读缓冲池 (lock-free)
│   ├── shared_frame_pool.h       # 帧缓冲池 (ABA-safe lock-free)
│   ├── work_queue.h              # io→biz 原始数据块队列
│   └── config.h                  # server_config
├── framework/                    # 业务框架
│   ├── application.h/cpp         # 应用脚手架（缓冲、拆帧、middleware 链、信号）
│   ├── service.h/cpp             # tcp_service 基类（含 codec_）
│   ├── middleware.h              # middleware 中间件基类
│   ├── dispatcher.h              # message_dispatcher 消息路由器
│   ├── codec/                    # 可插拔协议编解码
│   │   ├── codec.h               #   codec 基类
│   │   ├── length_prefixed.h     #   2B 大端长度头协议（默认）
│   │   └── raw_codec.h           #   透传协议
│   └── middleware/               # 内置中间件
│       ├── connection_limiter.h
│       ├── rate_limiter.h
│       └── access_log.h
├── examples/                     # 示例（namespace::run 接口）
│   ├── echo/
│   ├── chat/
│   ├── dispatcher/
│   └── http/                     # HTTP 服务（raw_codec，浏览器可直接访问）
├── tcpserver.cpp                 # 统一入口
├── CMakeLists.txt
└── vcpkg.json
```

## 配置参考

```cpp
struct server_config {
    std::string listen_ip   = "0.0.0.0";
    int listen_port         = 8899;
    int listen_backlog      = 65536;

    int io_threads          = 0;    // 0=auto (CPU*2/3)
    int biz_threads         = 0;    // 0=auto (CPU/3)

    int socket_rcvbuf       = 65536;
    int socket_sndbuf       = 65536;
    bool tcp_nodelay        = true;

    size_t max_payload      = 65535;
    size_t read_buf_size    = 65536;

    size_t write_queue_max_count = 1024;
    size_t write_queue_max_bytes = 262144;  // 256KB

    bool ordered_session    = true;   // 同 session 消息串行处理
    bool biz_threads_enabled = true;  // io/biz 分离
    bool cpu_affinity       = false;  // CPU 绑核
};
```

## 内置 Metrics

`server::metrics` 提供纯字节粒度的原子计数器（"消息"概念在 framework 层）：

| 指标 | 说明 |
|------|------|
| `bytes_received` | 收到字节总数 |
| `bytes_sent` | 发出字节总数 |
| `msg_dropped` | 背压丢弃数 |
| `connections` | 当前活跃连接数 |

每 5 秒自动打印监控日志，包含 IO 速率和内存池状态。
