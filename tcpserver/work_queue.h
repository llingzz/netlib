#pragma once
#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <string_view>
#include <vector>

class session;
class buffer_pool;

// 零拷贝 work_item: read_buf 直接从 buffer_pool 移交,
// payload 数据不复制, offs 直接索引 read_buf 中的原始数据
struct work_item {
    std::shared_ptr<session> sess;
    buffer_pool*             buf_pool = nullptr;  // 用于释放 read_buf
    char*                    read_buf = nullptr;  // 借出的读缓冲, 含所有 payload
    std::vector<uint16_t>    offs;                // 偶数=offset, 奇数=length
};

class work_queue
{
public:
    void push(work_item w)
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            items_.push_back(std::move(w));
        }
        cv_.notify_one();
    }

    work_item pop()
    {
        std::unique_lock<std::mutex> lk(mtx_);
        cv_.wait(lk, [this] {
            return !items_.empty() || shutdown_;
            }
        );
        if (shutdown_ && items_.empty()) {
            return {};
        }
        auto w = std::move(items_.front());
        items_.pop_front();
        return w;
    }

    void stop()
    {
        {
            std::lock_guard<std::mutex> lk(mtx_);
            shutdown_ = true;
        }
        cv_.notify_all();
    }

private:
    mutable std::mutex      mtx_;
    std::deque<work_item>   items_;
    std::condition_variable cv_;
    bool shutdown_ = false;
};
