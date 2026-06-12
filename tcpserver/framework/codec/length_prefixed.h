#pragma once
// ──── length_prefixed_codec: 2B 大端长度头协议 ────────────────
//
// 帧格式: [2B big-endian length][payload]
//
// 这是默认 codec, 向后兼容现有 echo/chat/dispatcher 示例。
// ───────────────────────────────────────────────────────────────

#include "codec.h"
#include <cstring>

#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <arpa/inet.h>
  #include <sys/socket.h>
#endif

class length_prefixed_codec : public codec {
public:
    static constexpr size_t HEADER_SIZE = 2;

    std::vector<std::pair<uint16_t, uint16_t>>
        decode(const char* data, size_t len, size_t& consumed) override
    {
        std::vector<std::pair<uint16_t, uint16_t>> msgs;
        size_t pos = 0;
        while (pos + HEADER_SIZE <= len) {
            uint16_t plen = read_header(data + pos);
            if (plen == 0 || pos + HEADER_SIZE + plen > len) {
                break;
            }
            msgs.emplace_back(
                static_cast<uint16_t>(pos + HEADER_SIZE),
                plen);
            pos += HEADER_SIZE + plen;
        }
        consumed = pos;
        return msgs;
    }

    std::string encode(std::string_view payload) override
    {
        std::string frame(HEADER_SIZE + payload.size(), '\0');
        char* p = frame.data();
        write_header(p, static_cast<uint16_t>(payload.size()));
        std::memcpy(p + HEADER_SIZE, payload.data(), payload.size());
        return frame;
    }

    size_t header_overhead() const override { return HEADER_SIZE; }

private:
    static void write_header(char* buf, uint16_t len)
    {
        uint16_t n = htons(len);
        std::memcpy(buf, &n, HEADER_SIZE);
    }

    static uint16_t read_header(const char* buf)
    {
        uint16_t n;
        std::memcpy(&n, buf, HEADER_SIZE);
        return ntohs(n);
    }
};
