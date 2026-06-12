// ──── BufferPool: 固定大小读缓冲池 (Lock-Free) ───────────────────────────
//
// 设计目标
//   - 每个 buffer 固定 64KB (kBufSize), 对应一条 TCP 连接的读缓冲
//   - 完全无锁 (lock-free), 基于 Treiber stack 的 CAS 自由链表
//   - acquire() 优先从自由链表弹出一个节点; 链表空时 new 新节点, 无容量上限
//   - release() 将用完的 buffer 归还链表, 不 delete, 实现零分配稳态
//
// 内存模型
//   - acquire(): CAS 使用 acquire 语义, 确保读到的是完整的 node->data
//   - release(): CAS 使用 release 语义, 确保 node->data 写入对后续 acquire 可见
//   - 负载/存储自由链表头指针用 relaxed, 由 CAS 自身的屏障保证正确性
//
// 节点布局
//   struct node {
//       node* next;        // 自由链表指针 (8B)
//       char  data[64KB];  // 读缓冲数据区
//   };
//   sizeof(node) = 8 + 65536 = 65544B, 约 64KB/节点
//   release() 通过 offsetof(node, data) 从 data 指针反推 node 指针
//
// 生命周期
//   - 构造: free_list_ = nullptr, total_allocated_ = 0
//   - 运行: session 调用 acquire() 获取读缓冲, 用完后 release() 归还
//   - 析构: 遍历自由链表 delete 所有节点, 释放全部堆内存
//
// 线程安全
//   - acquire() / release() 可被任意线程并发调用, 完全无锁
//   - total_allocated() / total_bytes_allocated() 为近似值, relaxed 读取
//
// 使用场景
//   - 每个 session 构造时 acquire() 一块 read_buf_, 析构时 release() 归还
//   - process_received_data() 分离模式下将旧 buf 移交 work_item, 再 acquire 新 buf
//   - 全局仅一个 buffer_pool 实例, 由 server 持有, 所有 session 共享
// ───────────────────────────────────────────────────────────────────────────
#pragma once
#include <atomic>
#include <cstddef>

class buffer_pool
{
public:
    static constexpr size_t kBufSize = 65536;

    char* acquire()
    {
        node* n = free_list_.load(std::memory_order_relaxed);
        while (n && !free_list_.compare_exchange_weak(n, n->next, std::memory_order_acquire, std::memory_order_relaxed)) {}
        if (!n) {
            n = new node();
            total_allocated_.fetch_add(1, std::memory_order_relaxed);
        }
        return n->data;
    }

    void release(char* p)
    {
        auto* n = reinterpret_cast<node*>(p - offsetof(node, data));
        n->next = free_list_.load(std::memory_order_relaxed);
        while (!free_list_.compare_exchange_weak(n->next, n, std::memory_order_release, std::memory_order_relaxed)) {}
    }

    ~buffer_pool()
    {
        node* n = free_list_.load();
        while (n) {
            auto* next = n->next;
            delete n; n = next;
        }
    }

    size_t total_allocated() const {
        return total_allocated_.load();
    }

    uint64_t total_bytes_allocated() const {
        return total_allocated_.load(std::memory_order_relaxed) * sizeof(node);
    }

private:
    struct node {
        node* next = nullptr;
        char data[kBufSize];
    };
    std::atomic<node*> free_list_{ nullptr };
    std::atomic<size_t> total_allocated_{ 0 };
};
