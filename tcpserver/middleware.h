#pragma once
// ──── middleware: 中间件基类 ───────────────────────────────────
//
// 四个钩子覆盖连接 + 消息两个维度:
//
//   连接级:
//     on_connect()     — 返回 false 拒绝连接, session 立即关闭
//     on_disconnect()  — 连接断开时清理
//
//   消息级:
//     before()         — 返回 false 丢弃消息, 不进入后续 middleware 和 service
//     after()          — 消息处理完成后 (逆序调用)
//
// 执行链:
//   新连接 → mw[0].on_connect → mw[1].on_connect → … → service.on_connect
//   消息   → mw[0].before    → mw[1].before    → … → service.on_message
//          → mw[1].after     → mw[0].after
//   断开   → service.on_disconnect → mw[0].on_disconnect → mw[1].on_disconnect
//
// 线程模型: 取决于 server 运行模式
//   - separate: on_connect 在 io 线程, before/after/on_message 在 biz 线程
//   - inline:   全部在 io 线程
// ───────────────────────────────────────────────────────────────

#include <string_view>

class session;

class middleware {
public:
    virtual ~middleware() = default;

    // ── 连接级 ──
    // 返回 false 拒绝连接
    virtual bool on_connect(session* sess) { return true; }
    virtual void on_disconnect(session* sess) {}

    // ── 消息级 ──
    // 返回 false 丢弃消息
    virtual bool before(session* sess, std::string_view msg) { return true; }
    virtual void after(session* sess, std::string_view msg) {}
};
