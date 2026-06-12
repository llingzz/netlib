// ──── netlib 统一入口 ──────────────────────────────────────
//
// 通过命令行参数选择要启动的服务模式。
//
// 用法:
//   tcpserver echo       [--port N] [--inline]
//   tcpserver chat       [--port N]
//   tcpserver dispatcher [--port N]
//   tcpserver --help
//
// 也可以直接在代码中 #include 各服务的头文件并调用:
//   echo::run(argc, argv);
//   chat::run(argc, argv);
//   dispatcher::run(argc, argv);
// ───────────────────────────────────────────────────────────────

#include "examples/echo/echo_service.h"
#include "examples/chat/chat_service.h"
#include "examples/dispatcher/dispatcher_service.h"

#include <spdlog/spdlog.h>
#include <cstring>
#include <iostream>
#include <string_view>

static void print_usage()
{
    std::cout <<
        "Usage:\n"
        "  tcpserver echo       [--port N] [--inline]     Echo server\n"
        "  tcpserver chat       [--port N]                Chat room server\n"
        "  tcpserver dispatcher [--port N]                Multi-type dispatcher demo\n"
        "  tcpserver --help                              Show this help\n";
}

int main(int argc, char* argv[])
{
    spdlog::set_level(spdlog::level::info);

    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string_view mode = argv[1];

    // 跳过程序名和 mode，将剩余参数传给各服务的 run()
    int  remaining_argc = argc - 1;
    char** remaining_argv = argv + 1;

    if (mode == "echo") {
        return echo::run(remaining_argc, remaining_argv);
    }
    else if (mode == "chat") {
        return chat::run(remaining_argc, remaining_argv);
    }
    else if (mode == "dispatcher") {
        return dispatcher::run(remaining_argc, remaining_argv);
    }
    else if (mode == "--help" || mode == "-h") {
        print_usage();
        return 0;
    }
    else {
        std::cerr << "Unknown mode: " << mode << "\n";
        print_usage();
        return 1;
    }
}
