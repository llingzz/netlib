#pragma once
// ──── tcp_service: 业务服务基类 ────────────────────────────────
//
// 用户继承此类实现业务逻辑。框架通过 application 自动注入 server*，
// 用户可直接调用 reply() / send() / server_ref()。
//
// 生命周期:
//   on_start()        — server 启动后
//   on_connect()      — 新连接建立 (middleware 全部通过后)
//   on_message()      — 收到消息 (纯虚, 必须实现)
//   on_disconnect()   — 连接断开
//   on_stop()         — server 停止前
//
// 线程模型: 取决于 server_config::biz_threads_enabled
//   - true  (separate): on_message 在 biz 线程池执行
//   - false (inline):   on_message 在 io 线程执行
// ───────────────────────────────────────────────────────────────

#include <cstdint>
#include <string>
#include <string_view>

class server;
class session;

class tcp_service {
public:
    virtual ~tcp_service() = default;

    // ── 生命周期 ──
    virtual void on_start(server& srv)    {}
    virtual void on_stop(server& srv)     {}

    // ── 连接事件 ──
    virtual void on_connect(session* sess)    {}
    virtual void on_disconnect(session* sess) {}

    // ── 消息处理 (纯虚) ──
    virtual void on_message(session* sess, std::string_view msg) = 0;

protected:
    // 回复当前会话
    void reply(session* sess, std::string_view data);

    // 向任意会话发送消息 (按 token)
    void send(uint64_t token, std::string_view data);

    // 获取 server 引用 (高级用法: 遍历 session 等)
    server& server_ref() { return *server_; }

private:
    server* server_ = nullptr;
    friend class application;
};
