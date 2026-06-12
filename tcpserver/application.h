#pragma once
// ──── application: 应用脚手架 ─────────────────────────────────
//
// 一站式启动 TCP 服务:
//   1. 创建 tcp_service 子类
//   2. 配置 server_config
//   3. 注册 middleware (可选)
//   4. 调用 run() — 阻塞直到 SIGINT/SIGTERM
//
// 示例 (Echo):
//   application app(cfg, std::make_unique<echo_service>());
//   return app.run();
//
// 示例 (带中间件):
//   application app(cfg, std::make_unique<chat_service>());
//   app.use(std::make_unique<connection_limiter>(1000));
//   app.use(std::make_unique<access_log>());
//   return app.run();
// ───────────────────────────────────────────────────────────────

#include <memory>
#include <vector>

#include "core/config.h"
#include "service.h"
#include "middleware.h"

class server;

class application {
public:
    application(server_config cfg, std::unique_ptr<tcp_service> svc);
    ~application();

    // 禁止拷贝/移动 (server 持有裸指针指向 service_)
    application(const application&) = delete;
    application& operator=(const application&) = delete;

    // 注册中间件 (按注册顺序执行)
    application& use(std::unique_ptr<middleware> mw);

    // 阻塞运行, 收到 SIGINT/SIGTERM 时优雅关闭
    // 返回 0: 正常退出
    int run();

    // 主动关闭 (可从信号处理或其它线程调用)
    void shutdown();

private:
    server_config                         cfg_;
    std::unique_ptr<tcp_service>          service_;
    std::vector<std::unique_ptr<middleware>> middlewares_;
    std::unique_ptr<server>               server_;
};
