#include "application.h"
#include "../core/server.h"
#include "../core/session.h"
#include "codec/codec.h"

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

void application::on_raw_data(session* sess, std::string_view data)
{
    auto token = sess->token();

    // ── 拼接到 per-session 缓冲区 ──
    auto& buf = pending_[token];
    buf.append(data.data(), data.size());

    // ── codec 拆帧 ──
    size_t consumed = 0;
    auto msgs = service_->codec_->decode(buf.data(), buf.size(), consumed);

    // ── middleware + on_message ──
    for (auto& [off, len] : msgs) {
        std::string_view msg(buf.data() + off, len);

        bool dropped = false;
        for (auto& mw : middlewares_) {
            if (!mw->before(sess, msg)) {
                dropped = true;
                break;
            }
        }
        if (!dropped) {
            service_->on_message(sess, msg);
            // after: 逆序
            for (auto it = middlewares_.rbegin(); it != middlewares_.rend(); ++it) {
                (*it)->after(sess, msg);
            }
        }
    }

    // ── 保留未消费的尾部 ──
    if (consumed > 0) {
        buf.erase(0, consumed);
    }
}

int application::run()
{
    // ── 编织回调链 ──

    auto on_connect = [this](session* sess) -> bool {
        for (auto& mw : middlewares_) {
            if (!mw->on_connect(sess)) return false;
        }
        service_->on_connect(sess);
        return true;
    };

    auto on_data = [this](session* sess, std::string_view data) {
        on_raw_data(sess, data);
    };

    auto on_disconnect = [this](session* sess) {
        auto token = sess->token();
        service_->on_disconnect(sess);
        for (auto& mw : middlewares_) {
            mw->on_disconnect(sess);
        }
        // 清理 per-session 缓冲区
        pending_.erase(token);
    };

    // ── 创建 server ──
    server_ = std::make_unique<server>(cfg_,
        std::move(on_data),
        std::move(on_disconnect),
        std::move(on_connect));
    service_->server_ = server_.get();

    // ── 信号处理 ──
    asio::signal_set signals(server_->io_ctx(0), SIGINT, SIGTERM);
    signals.async_wait([this](std::error_code ec, int sig) {
        if (!ec) {
            spdlog::info("Received signal {}, shutting down...", sig);
            server_->shutdown();
        }
    });

    // ── 启动 ──
    service_->on_start(*server_);
    server_->run();
    service_->on_stop(*server_);

    return 0;
}

void application::shutdown()
{
    if (server_) {
        server_->shutdown();
    }
}
