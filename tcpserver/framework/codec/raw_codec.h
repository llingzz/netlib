#pragma once
// ──── raw_codec: 透传协议 ─────────────────────────────────────
//
// 不做任何拆帧/组帧, 字节原样进出。
// 适用于 HTTP 等自描述协议 —— 上层 service 自行处理粘包分包。
//
// decode: 所有数据 = 一条消息, consumed = len
// encode:  原样返回 payload
// ───────────────────────────────────────────────────────────────

#include "codec.h"

class raw_codec : public codec {
public:
    std::vector<std::pair<uint16_t, uint16_t>>
        decode(const char* data, size_t len, size_t& consumed) override
    {
        consumed = len;
        // 全部数据 = 一条消息 (offset=0, length=len)
        return {{0, static_cast<uint16_t>(len)}};
    }

    std::string encode(std::string_view payload) override
    {
        return std::string(payload);
    }

    size_t header_overhead() const override { return 0; }
};
