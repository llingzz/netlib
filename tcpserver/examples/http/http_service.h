#pragma once
// ──── 简易 HTTP/1.1 服务 ────────────────────────────────────
//
// 基于框架实现的轻量 HTTP 服务器，支持:
//   - GET / POST 请求解析
//   - 路径路由注册
//   - 自定义 header / body 响应
//   - 静态 404 / 405 页面
//
// 说明:
//   底层使用 2 字节长度头帧协议。客户端发送 HTTP 请求前需
//   先发 2 字节大端长度。服务端回复同理。可用附带的测试脚本
//   或 printf + nc 测试。
//
// 用法:
//   #include "examples/http/http_service.h"
//   http_service::run(argc, argv);
// ───────────────────────────────────────────────────────────────

namespace http_service {

// 启动 HTTP 服务 (阻塞)
// 支持参数: --port N, --inline
int run(int argc, char* argv[]);

} // namespace http_service
