// ──── 聊天室服务实现 ────────────────────────────────────────

#include "chat_service.h"
#include "../../core/session.h"
#include "../../application.h"
#include "../../service.h"
#include "../../dispatcher.h"
#include "../../middleware/connection_limiter.h"
#include "../../middleware/access_log.h"

#include <spdlog/spdlog.h>
#include <cstdlib>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace chat {
namespace {

class chat_service : public tcp_service {
public:
    chat_service()
    {
        dispatch_.on(0x01, [this](session* s, std::string_view m) { on_nick(s, m); });
        dispatch_.on(0x02, [this](session* s, std::string_view m) { on_say(s, m);  });
        dispatch_.fallback([](session* s, std::string_view) {
            spdlog::warn("unknown msg type from token={:#x}", s->token());
        });
    }

    void on_message(session* sess, std::string_view msg) override
    {
        dispatch_.dispatch(sess, msg);
    }

    void on_disconnect(session* sess) override
    {
        std::string name;
        {
            std::lock_guard lk(mtx_);
            auto it = users_.find(sess->token());
            if (it != users_.end()) {
                name = it->second;
                users_.erase(it);
            }
        }
        if (!name.empty()) {
            spdlog::info("{} left the chat", name);
            broadcast(fmt::format("[system] {} left the room", name));
        }
    }

private:
    void on_nick(session* s, std::string_view nick)
    {
        std::string name(nick);
        {
            std::lock_guard lk(mtx_);
            users_[s->token()] = name;
        }
        spdlog::info("{} joined the chat", name);
        reply(s, fmt::format("[system] Welcome, {}!", name));
        broadcast(fmt::format("[system] {} joined the room", name));
    }

    void on_say(session* s, std::string_view text)
    {
        std::string name;
        {
            std::lock_guard lk(mtx_);
            auto it = users_.find(s->token());
            if (it == users_.end()) {
                reply(s, "[system] Please set your nickname first (0x01 + name)");
                return;
            }
            name = it->second;
        }
        broadcast(fmt::format("{}: {}", name, text));
    }

    void broadcast(std::string msg)
    {
        std::lock_guard lk(mtx_);
        for (auto& [token, _] : users_) {
            send(token, msg);
        }
    }

    message_dispatcher dispatch_;
    std::mutex mtx_;
    std::unordered_map<uint64_t, std::string> users_;
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

    spdlog::info("Chat server starting on port {}", cfg.listen_port);

    application app(cfg, std::make_unique<chat_service>());
    app.use(std::make_unique<connection_limiter>(1000));
    app.use(std::make_unique<access_log>());

    return app.run();
}

} // namespace chat
