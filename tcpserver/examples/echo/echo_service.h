#pragma once
// ──── Echo 服务: 最简单的框架使用示例 ────────────────────────
//
// 收到什么就回复什么。适合测试连通性和基准压测。
//
// 用法:
//   #include "examples/echo/echo_service.h"
//   echo::run(argc, argv);
// ───────────────────────────────────────────────────────────────

namespace echo {

// 启动 Echo 服务 (阻塞)
// 支持参数: --port N, --inline
int run(int argc, char* argv[]);

} // namespace echo
