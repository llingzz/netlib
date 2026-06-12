#pragma once
// ──── connection_limiter: 最大连接数限制 ──────────────────────
//
// 当活跃连接数 >= max_connections 时拒绝新连接。
//
// 用法:
//   app.use(std::make_unique<connection_limiter>(1000));
// ───────────────────────────────────────────────────────────────

#include "../middleware.h"
#include <atomic>
#include <cstdint>

class connection_limiter : public middleware {
public:
    explicit connection_limiter(int64_t max_connections)
        : max_(max_connections)
    {}

    bool on_connect(session* sess) override
    {
        (void)sess;
        int64_t cur = count_.fetch_add(1, std::memory_order_relaxed);
        if (cur >= max_) {
            count_.fetch_sub(1, std::memory_order_relaxed);
            return false;
        }
        return true;
    }

    void on_disconnect(session* sess) override
    {
        (void)sess;
        count_.fetch_sub(1, std::memory_order_relaxed);
    }

    int64_t current() const { return count_.load(std::memory_order_relaxed); }

private:
    int64_t max_;
    std::atomic<int64_t> count_{0};
};
