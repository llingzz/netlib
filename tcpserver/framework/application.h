#pragma once
// ──── application: 应用脚手架 ─────────────────────────────────
//
// 一站式启动 TCP 服务:
//   1. 创建 tcp_service 子类
//   2. 配置 server_config
//   3. 注册 middleware (可选)
//   4. 调用 run() — 阻塞直到 SIGINT/SIGTERM
//
// application 负责:
//   - 维护 per-session 缓冲区 (处理 TCP 粘包分包)
//   - 调用 codec 拆帧 → 投递完整消息
//   - 编织 middleware 链 + service 回调
//
// 示例 (Echo):
//   application app(cfg, std::make_unique<echo_service>());
//   return app.run();
//
// 示例 (HTTP / 带中间件):
//   application app(cfg, std::make_unique<http_service>());
//   app.use(std::make_unique<access_log>());
//   return app.run();
// ───────────────────────────────────────────────────────────────

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "../core/config.h"
#include "service.h"
#include "middleware.h"

class server;
class session;

class application {
public:
    application(server_config cfg, std::unique_ptr<tcp_service> svc);
    ~application();

    application(const application&) = delete;
    application& operator=(const application&) = delete;

    application& use(std::unique_ptr<middleware> mw);

    int run();
    void shutdown();

private:
    // 收到原始数据 → 缓冲 → codec 拆帧 → middleware + on_message
    void on_raw_data(session* sess, std::string_view data);

    server_config                         cfg_;
    std::unique_ptr<tcp_service>          service_;
    std::vector<std::unique_ptr<middleware>> middlewares_;
    std::unique_ptr<server>               server_;

    // per-session 拼接缓冲 (处理 TCP 粘包)
    std::unordered_map<uint64_t, std::string> pending_;
};
