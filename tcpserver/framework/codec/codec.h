#pragma once
// ──── codec: 协议编解码器接口 ─────────────────────────────────
//
// 定义"一条消息的边界"。session 负责从 socket 读字节并缓冲,
// codec 负责从字节流中找到消息边界 (拆帧) 和编码 (组帧)。
//
// 所有 codec 的实现必须是无状态的 —— decode() 可能被
// 不同线程并发调用 (不同 session 的 io 线程共享同一个 codec 实例)。
// ───────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

class codec {
public:
    virtual ~codec() = default;

    // 从字节流中提取消息。
    //   data:     已缓冲数据的起始指针
    //   len:      已缓冲字节数
    //   consumed: [out] 已消费的字节数 (完整帧)
    //   return:   (offset, length) 列表, offset 相对 data
    //
    // 调用方在 decode 后 memmove 保留 [consumed, len) 的未消费尾部,
    // 拼接到下一批数据的开头继续解码。
    virtual std::vector<std::pair<uint16_t, uint16_t>>
        decode(const char* data, size_t len, size_t& consumed) = 0;

    // 编码 payload 为完整帧 (含帧头帧尾)
    virtual std::string encode(std::string_view payload) = 0;

    // 帧头开销 (用于 SharedFramePool 预分配缓冲大小)
    virtual size_t header_overhead() const = 0;
};
