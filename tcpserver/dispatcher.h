#pragma once
// ──── message_dispatcher: 消息路由器 ──────────────────────────
//
// 按消息首字节 type_id 分发到注册的 handler。
// 不依赖框架其他组件, 可独立使用。
//
// 用法:
//   message_dispatcher d;
//   d.on(0x01, [](session* s, std::string_view payload) { ... });
//   d.on(0x02, [](session* s, std::string_view payload) { ... });
//   d.fallback([](session* s, std::string_view raw) {
//       spdlog::warn("unknown msg type");
//   });
//
//   void on_message(session* sess, std::string_view msg) override {
//       d.dispatch(sess, msg);
//   }
// ───────────────────────────────────────────────────────────────

#include <cstdint>
#include <functional>
#include <string_view>

class session;

class message_dispatcher {
public:
    // 注册 type_id 对应的 handler
    // handler 收到的 msg 已去掉首字节 type_id
    message_dispatcher& on(uint8_t type_id,
                           std::function<void(session*, std::string_view)> handler)
    {
        handlers_[type_id] = std::move(handler);
        return *this;
    }

    // 未匹配 type_id 时的回退 (可选)
    message_dispatcher& fallback(std::function<void(session*, std::string_view)> handler)
    {
        fallback_ = std::move(handler);
        return *this;
    }

    // 执行派发: 读 msg[0] → 查表 → 调用 handler
    void dispatch(session* sess, std::string_view msg)
    {
        if (msg.empty()) {
            if (fallback_) fallback_(sess, msg);
            return;
        }
        uint8_t type_id = static_cast<uint8_t>(msg[0]);
        auto& handler = handlers_[type_id];
        if (handler) {
            handler(sess, msg.substr(1));
        } else if (fallback_) {
            fallback_(sess, msg);
        }
    }

private:
    std::function<void(session*, std::string_view)> handlers_[256];
    std::function<void(session*, std::string_view)> fallback_;
};
