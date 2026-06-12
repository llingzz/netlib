#include "server.h"
#include "session.h"
#include <spdlog/spdlog.h>
#include <cstring>
#include <iostream>

// ──── Echo 消息处理器 ────────────────────────────────
static void on_message(session* sess, std::string_view msg)
{
    sess->reply(msg);
}

static void on_disconnect(session* sess)
{
    //spdlog::debug("Session disconnected: token={:#x}", sess->token());
}

// ──── Server 模式 ────────────────────────────────────
static int run_server(int argc, char* argv[])
{
    server_config cfg;
    cfg.listen_port = 8899;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg = argv[i];
        if (arg == "--port" && i + 1 < argc)
            cfg.listen_port = std::atoi(argv[++i]);
        else if (arg == "--ip" && i + 1 < argc)
            cfg.listen_ip = argv[++i];
        else if (arg == "--io-threads" && i + 1 < argc)
            cfg.io_threads = std::atoi(argv[++i]);
        else if (arg == "--biz-threads" && i + 1 < argc)
            cfg.biz_threads = std::atoi(argv[++i]);
        else if (arg == "--max-payload" && i + 1 < argc)
            cfg.max_payload = static_cast<uint16_t>(std::atoi(argv[++i]));
        else if (arg == "--inline" || arg == "--no-biz")
            cfg.biz_threads_enabled = false;
    }
    spdlog::info("Starting echo server on {}:{}", cfg.listen_ip, cfg.listen_port);
    spdlog::info("IO threads: cfg.io_threads={}, Biz threads: cfg.biz_threads={}, Mode: {}", cfg.io_threads, cfg.biz_threads, cfg.biz_threads_enabled ? "separate" : "inline");

    server srv(cfg, on_message, on_disconnect);
    srv.run();

    return 0;
}


// ──── Main ───────────────────────────────────────────
static void print_usage()
{
    std::cout <<
        "Usage:\n"
        "  netlib server [--port N] [--ip IP] [--io-threads N] [--biz-threads N]\n"
        "  netlib --help\n";
}

int main(int argc, char* argv[])
{
    spdlog::set_level(spdlog::level::debug);

    if (argc < 2) {
        print_usage();
        return 1;
    }

    std::string_view mode = argv[1];
    if (mode == "server") {
        return run_server(argc - 1, argv + 1);
    }
    else if (mode == "--help" || mode == "-h") {
        print_usage();
        return 0;
    }
    else {
        // 默认启动 server
        return run_server(argc, argv);
    }
}
