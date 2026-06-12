# tcpserver — 高性能 TCP 服务框架

基于 asio 的高并发 TCP 服务端框架，提供网络引擎 + 业务框架的分层架构，
方便二次开发各类 TCP 业务服务。

## 分层架构

```
┌──────────────────────────────────────────────────────┐
│               examples/ (示例 + 入口)                 │
├──────────────────────────────────────────────────────┤
│  tcp_service  │  middleware  │  message_dispatcher   │  ← Framework
├──────────────────────────────────────────────────────┤
│  application (脚手架: 编织回调链、信号处理、生命周期)   │
├──────────────────────────────────────────────────────┤
│  server  │  session  │  buffer_pool  │  protocol     │  ← Core
├──────────────────────────────────────────────────────┤
│              asio  │  spdlog                          │
└──────────────────────────────────────────────────────┘
```

### Core 层 — 网络引擎

提供高性能异步网络 IO，零拷贝数据路径。不感知业务逻辑。

| 组件 | 职责 |
|------|------|
| `server` | 多 io_context accept、io/biz 线程池、全局 metrics、监控定时器 |
| `session` | 单连接读写：拆帧 → 投递 → 编码 → 异步写，零拷贝 buffer 移交 |
| `buffer_pool` | 64KB 固定大小读缓冲池，lock-free Treiber stack |
| `shared_frame_pool` | 8 档 (512B~64KB) 帧缓冲池，ABA-safe tagged pointer，lock-free |
| `work_queue` | io → biz 线程的消息队列 |
| `protocol` | 2 字节大端长度头 + payload 的帧协议 |
| `config` | server_config 配置结构体 |

关键设计：
- **Inline 模式** (`biz_threads_enabled=false`): io 线程直接处理消息，零跨线程，适合 echo/转发
- **Separate 模式** (`biz_threads_enabled=true`): io/biz 线程分离，适合慢业务
- **零拷贝读路径**: `read_buf` 从 buffer_pool 获取，biz 处理完后归还
- **背压保护**: 写队列超限自动丢弃帧，防止 OOM

### Framework 层 — 业务框架

基于 Core 层构建，提供业务开发的抽象和脚手架。

| 组件 | 职责 |
|------|------|
| `tcp_service` | 业务服务基类 — 生命周期 + 消息处理 |
| `middleware` | 中间件基类 — 连接/消息横切关注点 |
| `message_dispatcher` | 消息路由器 — 按首字节 type_id 分发 |
| `application` | 应用脚手架 — 编织回调链、信号处理、启动/关闭 |

### Middleware — 内置中间件

| 中间件 | 类型 | 功能 |
|--------|------|------|
| `connection_limiter` | 连接级 | 最大连接数限制，超限拒绝新连接 |
| `rate_limiter` | 消息级 | Token bucket 单连接限速 |
| `access_log` | 连接级 | 统一打印连接/断开日志（含对端地址） |

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
```

### 命令行参数

| 参数 | 说明 | 适用模式 |
|------|------|----------|
| `--port N` | 监听端口 (默认 8899) | 全部 |
| `--inline` | 使用 inline 模式 | echo |

## 快速开始

### 三步写一个 TCP 服务

**1. 继承 `tcp_service`，实现 `on_message`：**

```cpp
#include "service.h"

struct my_service : tcp_service {
    void on_message(session* sess, std::string_view msg) override {
        // 处理消息
        reply(sess, "OK");  // 回复当前会话
    }
};
```

**2. 创建 `application` 并启动：**

```cpp
#include "application.h"

int main() {
    server_config cfg;
    cfg.listen_port = 9000;

    application app(cfg, std::make_unique<my_service>());
    return app.run();  // 阻塞，SIGINT/SIGTERM 优雅退出
}
```

**3. 编译运行：**

```bash
echo "hello" | nc localhost 9000
# → "OK"
```

### 使用中间件

```cpp
application app(cfg, std::make_unique<my_service>());

// 最多 1000 并发连接
app.use(std::make_unique<connection_limiter>(1000));

// 统一连接日志
app.use(std::make_unique<access_log>());

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
#include "dispatcher.h"

struct my_service : tcp_service {
    message_dispatcher dispatch_;

    my_service() {
        dispatch_
            .on(0x01, [this](session* s, auto m) { handle_login(s, m); })
            .on(0x02, [this](session* s, auto m) { handle_chat(s, m);  })
            .fallback([](session* s, auto m) {
                spdlog::warn("unknown msg type");
            });
    }

    void on_message(session* sess, std::string_view msg) override {
        dispatch_.dispatch(sess, msg);  // 读 msg[0] 查表分发
    }
    // ...
};
```

### 自定义中间件

继承 `middleware`，覆写需要的钩子：

```cpp
#include "middleware.h"

class ip_blacklist : public middleware {
    bool on_connect(session* sess) override {
        if (is_blocked(sess->remote_addr()))
            return false;  // 拒绝连接
        return true;
    }
};
```

## tcp_service 完整生命周期

```
on_start(server&)     — server 启动后
on_connect(session*)  — 新连接建立（middleware 全部通过后）
on_message(session*, string_view) — 收到消息（纯虚，必须实现）
on_disconnect(session*) — 连接断开
on_stop(server&)      — server 停止前
```

## 目录结构

```
tcpserver/
├── core/                    # 网络引擎
│   ├── server.h/cpp         # 多 io_context acceptor、线程池、metrics
│   ├── session.h/cpp        # 单连接读写、零拷贝 buffer 移交
│   ├── buffer_pool.h        # 读缓冲池 (lock-free)
│   ├── shared_frame_pool.h  # 帧缓冲池 (ABA-safe lock-free)
│   ├── work_queue.h         # io→biz 消息队列
│   ├── config.h             # server_config
│   └── protocol.h           # 帧协议 (2B 长度头 + payload)
├── service.h/cpp            # tcp_service 业务基类
├── middleware.h             # middleware 中间件基类
├── dispatcher.h             # message_dispatcher 消息路由器
├── application.h/cpp        # application 应用脚手架
├── middleware/              # 内置中间件
│   ├── connection_limiter.h
│   ├── rate_limiter.h
│   └── access_log.h
├── examples/                # 示例（namespace::run 接口，无 main）
│   ├── echo/
│   ├── chat/
│   └── dispatcher/
├── tcpserver.cpp            # 统一入口
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

    uint16_t max_payload    = 65535;
    size_t read_buf_size    = 65536;

    size_t write_queue_max_count = 1024;
    size_t write_queue_max_bytes = 262144;  // 256KB

    bool ordered_session    = true;   // 同 session 消息串行处理
    bool biz_threads_enabled = true;  // io/biz 分离
    bool cpu_affinity       = false;  // CPU 绑核
};
```

## 内置 Metrics

`server::metrics` 提供以下原子计数器：

| 指标 | 说明 |
|------|------|
| `msg_received` | 收到消息总数 |
| `msg_sent` | 发出回复总数 |
| `msg_dropped` | 背压丢弃消息数 |
| `connections` | 当前活跃连接数 |
| `bytes_received` | 收到字节总数 |
| `bytes_sent` | 发出字节总数 |

每 5 秒自动打印监控日志，包含 IO 速率和内存池状态。
