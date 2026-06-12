#pragma once
// ──── rate_limiter: 单连接限速 ────────────────────────────────
//
// 基于 token bucket 算法, 限制每条连接的消息速率。
// 超限消息直接丢弃 (before 返回 false)。
//
// 用法:
//   app.use(std::make_unique<rate_limiter>(100));  // 每秒最多 100 条消息
// ───────────────────────────────────────────────────────────────

#include "../middleware.h"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <unordered_map>

class rate_limiter : public middleware {
public:
    explicit rate_limiter(size_t max_per_second)
        : max_per_sec_(max_per_second)
    {}

    bool before(session* sess, std::string_view msg) override
    {
        (void)msg;
        auto now = clock::now();
        auto token = sess->token();

        std::lock_guard<std::mutex> lk(mtx_);
        auto& bucket = buckets_[token];

        // Token bucket refill
        auto elapsed = now - bucket.last_refill;
        int64_t tokens_to_add = static_cast<int64_t>(
            std::chrono::duration_cast<std::chrono::microseconds>(elapsed).count()
            * max_per_sec_ / 1'000'000);
        if (tokens_to_add > 0) {
            bucket.tokens = std::min(bucket.tokens + tokens_to_add,
                                     static_cast<int64_t>(max_per_sec_));
            bucket.last_refill = now;
        }

        if (bucket.tokens <= 0) {
            return false;  // 丢弃
        }
        bucket.tokens--;
        return true;
    }

    void on_disconnect(session* sess) override
    {
        std::lock_guard<std::mutex> lk(mtx_);
        buckets_.erase(sess->token());
    }

private:
    using clock = std::chrono::steady_clock;

    struct bucket {
        int64_t tokens = 0;
        clock::time_point last_refill = clock::now();
    };

    size_t max_per_sec_;
    std::mutex mtx_;
    std::unordered_map<uint64_t, bucket> buckets_;
};
