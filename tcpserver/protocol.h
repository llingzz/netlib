#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>

#ifdef _WIN32
  #include <winsock2.h>
#else
  #include <arpa/inet.h>
  #include <sys/socket.h>
#endif

namespace protocol {

constexpr size_t HEADER_SIZE = 2;

inline void write_header(char* buf, uint16_t len)
{
    uint16_t n = htons(len);
    std::memcpy(buf, &n, HEADER_SIZE);
}

inline uint16_t read_header(const char* buf)
{
    uint16_t n;
    std::memcpy(&n, buf, HEADER_SIZE);
    return ntohs(n);
}

inline std::string encode(std::string_view payload)
{
    std::string frame(HEADER_SIZE + payload.size(), '\0');
    char* p = frame.data();
    write_header(p, static_cast<uint16_t>(payload.size()));
    std::memcpy(p + HEADER_SIZE, payload.data(), payload.size());
    return frame;
}

// 写入预分配 buffer，返回总帧长
inline uint16_t encode_into(std::string_view payload, char* buf)
{
    uint16_t plen = static_cast<uint16_t>(payload.size());
    write_header(buf, plen);
    std::memcpy(buf + HEADER_SIZE, payload.data(), plen);
    return HEADER_SIZE + plen;
}

} // namespace protocol
