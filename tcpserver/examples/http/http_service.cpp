// ──── 简易 HTTP/1.1 服务实现 ────────────────────────────────
//
// 使用 raw_codec —— session 不做协议解析, 直接透传 TCP 字节流,
// 因此浏览器可直接访问 http://localhost:8899。
//
// HTTP 协议自带 framing (Request-Line + Headers + Content-Length),
// 本服务自行维护 per-session 缓冲区处理粘包分包。
//
// 启动: tcpserver http --port 8899
// 测试: 浏览器打开 http://localhost:8899
// ───────────────────────────────────────────────────────────────

#include "http_service.h"
#include "../../framework/application.h"
#include "../../framework/service.h"
#include "../../framework/codec/raw_codec.h"
#include "../../core/session.h"

#include <spdlog/spdlog.h>
#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace http_service {
namespace {

// ═══════════════════════════════════════════════════════════════
// HTTP Request 解析器
// ═══════════════════════════════════════════════════════════════

struct http_request {
    std::string method;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;

    std::string header(std::string_view key) const {
        for (auto& [k, v] : headers) {
            if (iequals(k, key)) return v;
        }
        return {};
    }

private:
    static bool iequals(std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
            std::equal(a.begin(), a.end(), b.begin(),
                [](char x, char y) { return std::tolower(x) == std::tolower(y); });
    }
};

// 解析 HTTP 请求。返回 nullopt 表示数据不完整 (等更多数据)。
// 返回 req 表示成功解析一条完整请求。
// consumed: [out] 已消费字节数。
std::optional<http_request> parse_request(const char* data, size_t len, size_t& consumed)
{
    if (len < 14) return std::nullopt;

    const char* p = data;
    const char* end = p + len;

    // ── 找 header 结束标记 \r\n\r\n ──
    const char* header_end = nullptr;
    for (const char* s = p; s + 3 <= end; ++s) {
        if (s[0] == '\r' && s[1] == '\n' && s[2] == '\r' && s[3] == '\n') {
            header_end = s;
            break;
        }
    }
    if (!header_end) return std::nullopt;  // header 还不完整

    std::string_view headers_block(p, header_end - p);

    // ── Request Line ──
    size_t nl = headers_block.find("\r\n");
    if (nl == std::string_view::npos) return std::nullopt;
    std::string_view req_line = headers_block.substr(0, nl);

    size_t sp1 = req_line.find(' ');
    size_t sp2 = req_line.find(' ', sp1 + 1);
    if (sp1 == std::string_view::npos || sp2 == std::string_view::npos)
        return std::nullopt;

    http_request req;
    req.method  = req_line.substr(0, sp1);
    req.path    = req_line.substr(sp1 + 1, sp2 - sp1 - 1);
    req.version = req_line.substr(sp2 + 1);

    // ── Headers ──
    size_t hpos = nl + 2;
    while (hpos < headers_block.size()) {
        size_t hn = headers_block.find("\r\n", hpos);
        if (hn == std::string_view::npos) break;
        std::string_view line = headers_block.substr(hpos, hn - hpos);
        hpos = hn + 2;
        if (line.empty()) break;

        size_t col = line.find(':');
        if (col != std::string_view::npos) {
            std::string key(line.substr(0, col));
            std::string_view val = line.substr(col + 1);
            if (!val.empty() && val[0] == ' ') val.remove_prefix(1);
            req.headers[std::move(key)] = val;
        }
    }

    // ── Body ──
    size_t body_start = (header_end - data) + 4;
    size_t content_length = 0;
    auto it = req.headers.find("Content-Length");
    if (it != req.headers.end()) content_length = std::stoul(it->second);

    if (body_start + content_length > len) return std::nullopt;  // body 不完整

    req.body.assign(data + body_start, content_length);
    consumed = body_start + content_length;
    return req;
}

// ═══════════════════════════════════════════════════════════════
// HTTP Response 构造器
// ═══════════════════════════════════════════════════════════════

class http_response {
public:
    http_response() : status_line_("HTTP/1.1 200 OK") {}

    static http_response ok(std::string_view body,
                            std::string_view content_type = "text/html; charset=utf-8") {
        http_response r;
        r.status(200, "OK").body(body, content_type);
        return r;
    }
    static http_response json(std::string_view body) {
        return ok(body, "application/json; charset=utf-8");
    }
    static http_response plain(std::string_view body) {
        return ok(body, "text/plain; charset=utf-8");
    }
    static http_response not_found() {
        http_response r;
        r.status(404, "Not Found").body("<h1>404 Not Found</h1>");
        return r;
    }
    static http_response bad_request(std::string_view msg = "Bad Request") {
        http_response r;
        r.status(400, "Bad Request").body(msg, "text/plain");
        return r;
    }
    static http_response method_not_allowed() {
        http_response r;
        r.status(405, "Method Not Allowed").body("<h1>405 Method Not Allowed</h1>");
        return r;
    }
    static http_response server_error(std::string_view msg = "Internal Server Error") {
        http_response r;
        r.status(500, "Internal Server Error").body(msg, "text/plain");
        return r;
    }
    static http_response redirect(std::string_view location) {
        http_response r;
        r.status(302, "Found").header("Location", location);
        return r;
    }

    http_response& status(int code, std::string_view message) {
        char buf[64];
        std::snprintf(buf, sizeof(buf), "HTTP/1.1 %d %.*s",
            code, static_cast<int>(message.size()), message.data());
        status_line_ = buf;
        return *this;
    }
    http_response& header(std::string_view key, std::string_view value) {
        headers_.emplace_back(key, value);
        return *this;
    }
    http_response& body(std::string_view content,
                        std::string_view content_type = "text/html; charset=utf-8") {
        body_ = content;
        header("Content-Type", content_type);
        return *this;
    }

    std::string to_string() const {
        std::string result;
        result.reserve(status_line_.size() + headers_.size() * 48 + body_.size() + 8);
        result += status_line_;
        result += "\r\n";
        bool has_cl = false;
        for (auto& [k, v] : headers_) {
            if (iequals(k, "Content-Length")) has_cl = true;
            result += k; result += ": "; result += v; result += "\r\n";
        }
        if (!has_cl && !body_.empty()) {
            result += "Content-Length: ";
            result += std::to_string(body_.size());
            result += "\r\n";
        }
        result += "\r\n";
        result += body_;
        return result;
    }

private:
    static bool iequals(std::string_view a, std::string_view b) {
        return a.size() == b.size() &&
            std::equal(a.begin(), a.end(), b.begin(),
                [](char x, char y) { return std::tolower(x) == std::tolower(y); });
    }

    std::string status_line_;
    std::vector<std::pair<std::string, std::string>> headers_;
    std::string body_;
};

// ═══════════════════════════════════════════════════════════════
// HTTP Router
// ═══════════════════════════════════════════════════════════════

class http_router {
public:
    using handler_t = std::function<http_response(const http_request&)>;

    http_router& get(std::string_view path, handler_t h) {
        routes_.push_back({std::string(path), "GET", std::move(h)});
        return *this;
    }
    http_router& post(std::string_view path, handler_t h) {
        routes_.push_back({std::string(path), "POST", std::move(h)});
        return *this;
    }

    http_response handle(const http_request& req) const {
        for (auto& r : routes_) {
            if (r.path == req.path) {
                if (r.method != req.method) return http_response::method_not_allowed();
                return r.handler(req);
            }
        }
        // 前缀匹配
        for (auto& r : routes_) {
            if (r.path.size() > 1 && r.path.back() == '*') {
                std::string_view prefix(r.path.data(), r.path.size() - 1);
                if (req.path.size() >= prefix.size() &&
                    req.path.compare(0, prefix.size(), prefix) == 0) {
                    if (r.method != req.method) return http_response::method_not_allowed();
                    return r.handler(req);
                }
            }
        }
        return http_response::not_found();
    }

private:
    struct route { std::string path; std::string method; handler_t handler; };
    std::vector<route> routes_;
};

// ═══════════════════════════════════════════════════════════════
// HTTP Service — 使用 raw_codec, 自行处理 HTTP framing
// ═══════════════════════════════════════════════════════════════

class http_server_service : public tcp_service {
public:
    http_server_service()
    {
        // 关键: 替换 codec 为 raw, 透传 TCP 字节流
        codec_ = std::make_unique<raw_codec>();
        setup_routes();
    }

    void on_message(session* sess, std::string_view msg) override
    {
        // raw_codec 将每次收到的数据作为一条消息投递
        // 需要自行拼接和解析 HTTP 请求
        auto& buf = http_buf_[sess->token()];
        buf.append(msg.data(), msg.size());

        // 循环解析: 一个 TCP 帧可能包含多个 HTTP 请求 (pipeline)
        while (true) {
            size_t consumed = 0;
            auto req_opt = parse_request(buf.data(), buf.size(), consumed);
            if (!req_opt) break;  // 数据不完整, 等更多数据

            auto& req = *req_opt;
            spdlog::info("[http] {} {} from {}", req.method, req.path, sess->remote_addr());

            http_response resp;
            try {
                resp = router_.handle(req);
            } catch (const std::exception& e) {
                spdlog::error("HTTP handler exception: {}", e.what());
                resp = http_response::server_error(e.what());
            } catch (...) {
                resp = http_response::server_error();
            }

            // 直接写原始字节 (HTTP 响应自带 framing, 不需要 codec 编码)
            auto resp_str = resp.to_string();
            reply_raw(sess, resp_str);

            // 移除已消费的数据
            buf.erase(0, consumed);
        }
    }

    void on_disconnect(session* sess) override
    {
        http_buf_.erase(sess->token());
    }

private:
    void setup_routes()
    {
        router_.get("/", [](const http_request&) {
            return http_response::ok(
                "<!DOCTYPE html>\n"
                "<html><head><title>netlib HTTP</title></head>\n"
                "<body>\n"
                "  <h1>netlib HTTP Server</h1>\n"
                "  <p>A simple HTTP server built on the netlib TCP framework.</p>\n"
                "  <ul>\n"
                "    <li><a href='/hello'>/hello</a> — greeting</li>\n"
                "    <li><a href='/api/info'>/api/info</a> — server info (JSON)</li>\n"
                "    <li><a href='/echo?msg=hi'>/echo?msg=hi</a> — echo query param</li>\n"
                "  </ul>\n"
                "</body></html>\n"
            );
        });

        router_.get("/hello", [](const http_request&) {
            return http_response::ok("<h1>Hello, World!</h1>");
        });

        router_.get("/api/info", [](const http_request&) {
            return http_response::json(
                "{\n"
                "  \"server\": \"netlib-http\",\n"
                "  \"version\": \"1.0\",\n"
                "  \"framework\": \"tcpserver\"\n"
                "}\n"
            );
        });

        router_.get("/echo", [](const http_request& req) {
            std::string msg = "nothing to echo";
            size_t q = req.path.find('?');
            if (q != std::string::npos) {
                std::string_view query(req.path.data() + q + 1,
                    req.path.size() - q - 1);
                if (query.size() >= 4 && query.substr(0, 4) == "msg=") {
                    msg = std::string(query.substr(4));
                }
            }
            return http_response::plain(msg);
        });

        router_.post("/api/echo", [](const http_request& req) {
            return http_response::json(req.body);
        });

        router_.get("/redirect", [](const http_request&) {
            return http_response::redirect("/hello");
        });
    }

    http_router router_;
    std::unordered_map<uint64_t, std::string> http_buf_;
};

} // anonymous namespace

// ═══════════════════════════════════════════════════════════════
// 公开接口
// ═══════════════════════════════════════════════════════════════

int run(int argc, char* argv[])
{
    spdlog::set_level(spdlog::level::info);

    server_config cfg;
    cfg.listen_port = 8899;
    cfg.max_payload = 65535;

    for (int i = 0; i < argc; ++i) {
        std::string_view arg = argv[i];
        if ((arg == "--port" || arg == "-p") && i + 1 < argc)
            cfg.listen_port = std::atoi(argv[++i]);
        else if (arg == "--inline")
            cfg.biz_threads_enabled = false;
    }

    spdlog::info("HTTP server starting on http://localhost:{} (mode: {})",
        cfg.listen_port,
        cfg.biz_threads_enabled ? "separate" : "inline");

    application app(cfg, std::make_unique<http_server_service>());
    return app.run();
}

} // namespace http_service
