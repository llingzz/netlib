#pragma once
// ──── Dispatcher 演示: 多消息类型路由 ────────────────────────
//
// 展示 message_dispatcher 的独立使用方式:
//   - 注册多种消息类型
//   - fallback 处理未知类型
//   - service 层只需一行 dispatch_.dispatch()
//
// 协议 (每消息首字节为 type):
//   0x01 — 回显 (echo)
//   0x02 — 转大写 (uppercase)
//   0x03 — 获取统计 (stats)
//   其他 — 未知类型, 通过 fallback 返回提示
//
// 用法:
//   #include "examples/dispatcher/dispatcher_service.h"
//   dispatcher::run(argc, argv);
// ───────────────────────────────────────────────────────────────

namespace dispatcher {

// 启动 Dispatcher 演示服务 (阻塞)
// 支持参数: --port N
int run(int argc, char* argv[]);

} // namespace dispatcher
