#pragma once
// ──── 聊天室服务: 演示多用户广播 + middleware 使用 ──────────
//
// 协议 (每消息首字节为 type):
//   0x01 + nickname     — 设置昵称, 加入聊天室
//   0x02 + text         — 发送消息, 广播给所有人
//
// 用法:
//   #include "examples/chat/chat_service.h"
//   chat::run(argc, argv);
// ───────────────────────────────────────────────────────────────

namespace chat {

// 启动聊天室服务 (阻塞)
// 支持参数: --port N
int run(int argc, char* argv[]);

} // namespace chat
