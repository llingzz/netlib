#pragma once
// ──── access_log: 统一连接/断开日志 ───────────────────────────
//
// 自动打印每个连接的建立和断开, 包含对端地址和 token。
//
// 用法:
//   app.use(std::make_unique<access_log>());
// ───────────────────────────────────────────────────────────────

#include "../../core/session.h"
#include "../middleware.h"
#include <spdlog/spdlog.h>

class access_log : public middleware {
public:
    bool on_connect(session* sess) override
    {
        spdlog::info("[connect] addr={} token={:#x}", sess->remote_addr(), sess->token());
        return true;
    }

    void on_disconnect(session* sess) override
    {
        spdlog::info("[disconnect] token={:#x}", sess->token());
    }
};
