#include "application.h"
#include "core/server.h"
#include "core/session.h"

#include <asio.hpp>
#include <spdlog/spdlog.h>

application::application(server_config cfg, std::unique_ptr<tcp_service> svc)
    : cfg_(std::move(cfg))
    , service_(std::move(svc))
{
}

application::~application()
{
    shutdown();
}

application& application::use(std::unique_ptr<middleware> mw)
{
    middlewares_.push_back(std::move(mw));
    return *this;
}

int application::run()
{
    // ── 编织回调链: middleware + service → server 所需的三组回调 ──

    auto on_connect = [this](session* sess) -> bool {
        for (auto& mw : middlewares_) {
            if (!mw->on_connect(sess)) {
                return false;  // 中间件拒绝, server 层会关闭连接
            }
        }
        service_->on_connect(sess);
        return true;
    };

    auto on_msg = [this](session* sess, std::string_view msg) {
        // before: 任一中间件返回 false 则丢弃消息
        for (auto& mw : middlewares_) {
            if (!mw->before(sess, msg)) {
                return;
            }
        }
        service_->on_message(sess, msg);
        // after: 逆序调用
        for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
            (*it)->after(sess, msg);
        }
    };

    auto on_disconnect = [this](session* sess) {
        service_->on_disconnect(sess);
        for (auto& mw : middlewares_) {
            mw->on_disconnect(sess);
        }
    };

    // ── 创建 server ──
    server_ = std::make_unique<server>(cfg_,
        std::move(on_msg),
        std::move(on_disconnect),
        std::move(on_connect));
    service_->server_ = server_.get();

    // ── 信号处理 (asio signal_set, 线程安全) ──
    asio::signal_set signals(server_->io_ctx(0), SIGINT, SIGTERM);
    signals.async_wait([this](std::error_code ec, int sig) {
        if (!ec) {
            spdlog::info("Received signal {}, shutting down...", sig);
            server_->shutdown();
        }
    });

    // ── 启动 ──
    service_->on_start(*server_);
    server_->run();  // 阻塞直到 shutdown

    service_->on_stop(*server_);
    return 0;
}

void application::shutdown()
{
    if (server_) {
        server_->shutdown();
    }
}
