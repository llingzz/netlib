// ──── Dispatcher 演示服务实现 ────────────────────────────────

#include "dispatcher_service.h"
#include "../../application.h"
#include "../../service.h"
#include "../../dispatcher.h"
#include "../../middleware/access_log.h"

#include <spdlog/spdlog.h>
#include <atomic>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>

namespace dispatcher {
namespace {

class dispatcher_demo_service : public tcp_service {
public:
    dispatcher_demo_service()
    {
        dispatch_
            .on(0x01, [this](session* s, std::string_view m) {
                count_.fetch_add(1, std::memory_order_relaxed);
                reply(s, m);
            })
            .on(0x02, [this](session* s, std::string_view m) {
                count_.fetch_add(1, std::memory_order_relaxed);
                std::string upper(m);
                for (auto& c : upper) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
                reply(s, upper);
            })
            .on(0x03, [this](session* s, std::string_view) {
                count_.fetch_add(1, std::memory_order_relaxed);
                auto n = count_.load(std::memory_order_relaxed);
                reply(s, fmt::format("stats: {} messages processed", n));
            })
            .fallback([](session* s, std::string_view) {
                s->reply("unknown message type (use 0x01=echo 0x02=upper 0x03=stats)");
            });
    }

    void on_message(session* sess, std::string_view msg) override
    {
        dispatch_.dispatch(sess, msg);
    }

private:
    message_dispatcher dispatch_;
    std::atomic<int64_t> count_{0};
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
    }

    spdlog::info("Dispatcher demo server starting on port {}", cfg.listen_port);

    application app(cfg, std::make_unique<dispatcher_demo_service>());
    app.use(std::make_unique<access_log>());

    return app.run();
}

} // namespace dispatcher
