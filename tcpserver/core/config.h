#pragma once
#include <cstdint>
#include <string>

struct server_config
{
    // 监听地址
    std::string listen_ip   = "0.0.0.0";
    int listen_port = 8899;
    int listen_backlog = 65536;

    // 线程数 (0 = auto: io_threads=CPU*2/3 biz_threads=CPU/3)
    int io_threads  = 0;
    int biz_threads = 0;

    // Socket 参数
    int socket_rcvbuf = 65536;
    int socket_sndbuf = 65536;
    bool tcp_nodelay = true;
    int idle_timeout_sec = 300;

    // 消息限制
    size_t max_payload = 65535;
    size_t read_buf_size = 65536;

    // 写队列限制
    size_t write_queue_max_count = 1024;
    size_t write_queue_max_bytes = 262144;

    // 保序: true=同session消息串行处理, false=任意biz线程并行
    bool ordered_session = true;

    // false: 直接在 io 线程处理消息 (无 biz 线程, 零跨线程开销) 适合 echo/转发等轻量 handler
    // true:  io/biz 分离 (默认), 适合慢业务, 单个慢 handler 不影响其他连接
    bool biz_threads_enabled = true;

    // CPU 绑核
    bool cpu_affinity = false;
};
