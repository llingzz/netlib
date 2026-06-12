#pragma once
// ──── tcp_service: 业务服务基类 ────────────────────────────────
//
// 用户继承此类实现业务逻辑。框架自动注入 server* 和 codec。
//
// 生命周期:
//   on_start()        — server 启动后
//   on_connect()      — 新连接建立 (middleware 全部通过后)
//   on_message()      — 收到完整消息 (纯虚, codec 拆帧后)
//   on_disconnect()   — 连接断开
//   on_stop()         — server 停止前
//
// codec:
//   默认使用 length_prefixed_codec (2B 大端长度头)。
//   子类可在构造函数中替换 codec_ (如 HTTP 服务用 raw_codec)。
//
// 线程模型: 取决于 server_config::biz_threads_enabled
//   - true  (separate): on_message 在 biz 线程池执行
//   - false (inline):   on_message 在 io 线程执行
// ───────────────────────────────────────────────────────────────

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

class server;
class session;
class codec;

class tcp_service {
public:
    tcp_service();
    virtual ~tcp_service();

    // ── 生命周期 ──
    virtual void on_start(server& srv)    {}
    virtual void on_stop(server& srv)     {}

    // ── 连接事件 ──
    virtual void on_connect(session* sess)    {}
    virtual void on_disconnect(session* sess) {}

    // ── 消息处理 (纯虚) ──
    virtual void on_message(session* sess, std::string_view msg) = 0;

protected:
    // 回复当前会话 (走 codec 编码)
    void reply(session* sess, std::string_view payload);

    // 回复原始字节 (不走 codec, 直接写入 socket)
    void reply_raw(session* sess, std::string_view data);

    // 向任意会话发送 (走 codec)
    void send(uint64_t token, std::string_view payload);

    // 获取 server 引用
    server& server_ref() { return *server_; }

    // 协议编解码器 (子类可在构造函数中替换)
    std::unique_ptr<codec> codec_;

private:
    server* server_ = nullptr;
    friend class application;
};
