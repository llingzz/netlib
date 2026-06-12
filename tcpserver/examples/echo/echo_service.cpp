// ──── Echo 服务实现 ──────────────────────────────────────────

#include "echo_service.h"
#include "../../framework/application.h"
#include "../../framework/service.h"
#include <spdlog/spdlog.h>
#include <cstdlib>
#include <string_view>

namespace echo {
namespace {

struct echo_service : tcp_service {
    void on_message(session* sess, std::string_view msg) override
    {
        reply(sess, msg);
    }
};

} // anonymous namespace

int run(int argc, char* argv[])
{
    spdlog::set_level(spdlog::level::info);

    server_config cfg;
    cfg.listen_port = 8899;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            cfg.listen_port = std::atoi(argv[++i]);
        else if (arg == "--inline")
            cfg.biz_threads_enabled = false;
    }

    spdlog::info("Echo server starting on port {} (mode: {})",
        cfg.listen_port,
        cfg.biz_threads_enabled ? "separate" : "inline");

    application app(cfg, std::make_unique<echo_service>());
    return app.run();
}

} // namespace echo
