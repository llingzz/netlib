// ──── SharedFramePool: 分档帧缓冲池 (ABA-Safe Lock-Free) ──────────────────
//
// 设计目标
//   - 8 个 power-of-2 档位: 512B / 1K / 2K / 4K / 8K / 16K / 32K / 64K
//   - 每档位独立 lock-free Treiber stack, MPMC 安全
//   - acquire(payload_size) 自动向上取整到匹配档位, 永远返回有效指针
//   - release(data, frame_len) 自动路由回对应档位, 不 delete, 零分配稳态
//   - 自动扩容: freelist 空且 allocated >= current_cap 时 cap 翻倍
//
// ──── ABA 保护机制 ────────────────────────────────────────────────────────
//
// 问题: 经典 Treiber stack 的 CAS pop 存在 ABA 窗口:
//   1. 线程 A 读到栈顶节点 X, 记下 X->next = Y
//   2. 线程 B 连续 pop X → pop Y → push X (X 被复用)
//   3. 线程 A 的 CAS(X, Y) 成功, 但栈顶实际已是 X→Z, Y 已不在栈中
//   4. 结果: Z 丢失, 内存泄漏
//
// 解法: Tagged Pointer — 利用 x64 虚拟地址仅使用低 48 位的特性,
//       高 16 位用作 tag 计数器。每次 CAS 成功时 tag++,
//       使得即使同一节点地址被复用, tagged pointer 值也不同, ABA 窗口关闭。
//
//       布局: [63:48 tag] [47:0 ptr]
//       kPtrMask = 0x0000FFFFFFFFFFFF
//
//       untag_ptr(tagged) → tagged & kPtrMask       (提取裸指针)
//       untag_tag(tagged) → tagged >> 48             (提取当前 tag)
//       make_tagged(ptr, tag) → (tag<<48) | (ptr & kPtrMask)  (构造 tagged pointer)
//
//   free_list_ 存储带 tag 的指针, CAS 比较完整的 64 位值
//   node->next 存储不带 tag 的裸指针 (仅在节点被独占持有/准备归还时访问)
//
// ──── Treiber Stack 操作流程 ──────────────────────────────────────────────
//
//   acquire_from(ci):
//     1. 从 cp.freelist 读出 tagged pointer
//     2. untag_ptr 得到栈顶 node, untag_tag 得到当前 tag
//     3. 若 node == nullptr → freelist 空 → 扩容/分配新节点
//     4. 读 node->next (裸指针), 构造 new_tagged = make_tagged(next, tag+1)
//     5. CAS(cp.freelist, tagged → new_tagged)
//        - 成功: node 已从栈中移除, 返回 node->data
//        - 失败: tagged 被更新(其他线程已 pop/push), 循环重试
//
//   release_to(ci, data):
//     1. data - sizeof(void*) 还原出 node_raw (data 前 8 字节是 next 指针槽)
//     2. 读 cp.freelist 得到当前 tagged
//     3. 将 node_raw->next 指向当前栈顶 (untag_ptr(tagged))
//     4. 构造 new_tagged = make_tagged(node_raw, tag+1)
//     5. CAS(cp.freelist, tagged → new_tagged)
//        - 成功: node 已入栈, tag 已递增, 返回
//        - 失败: 循环重试
//
// ──── 节点布局 ────────────────────────────────────────────────────────────
//
//   struct node {
//       void* next;             // 自由链表裸指针 (8B), 无 tag
//       char  data[frame_size]; // 帧数据区, 大小由档位决定 (512B~64KB)
//   };
//
//   acquire() 返回 data 指针 (= node_raw + 8)
//   release() 通过 data - 8 还原 node_raw, 读写其 next 字段
//
// ──── 扩容策略 ────────────────────────────────────────────────────────────
//
//   - 初始 cap 逐档递减: 256/128/64/32/16/8/4/2 (小帧高频, 预分配更多)
//   - freelist 空时检查 allocated >= current_cap
//   - 若达到上限 → current_cap *= 2 (翻倍), 打印 info 日志
//   - 每次都 new 一个新节点 (不做批量预分配)
//   - 扩容只增不减, 峰值后不收缩
//
// ──── 帧长度路由 ──────────────────────────────────────────────────────────
//
//   round_up_frame(need): 向上取整到最近的 2 的幂 (512~65536)
//     例: 100B→512, 600B→1024, 5000B→8192, 40000B→65536
//
//   class_index(frame_size): switch 查表映射到 0~7
//     512→0, 1024→1, ..., 65536→7
//
// ──── 线程安全 ────────────────────────────────────────────────────────────
//
//   - acquire()/release() 可被任意线程并发调用, 完全无锁
//   - ABA-safe: tagged pointer 确保即使节点复用, CAS 也不会误成功
//   - MPMC: 多个生产者(业务线程 release) + 多个消费者(io 线程 acquire)安全
//   - total_bytes_allocated()/metrics() 为近似值, relaxed 读取
//
// ──── 使用场景 ────────────────────────────────────────────────────────────
//
//   - session::reply() 调用 acquire(data.size()) 获取帧缓冲
//   - protocol::encode_into() 将 payload 写入缓冲, 返回帧总长度
//   - session::do_write() 完成异步写后 release(fb.data, fb.len) 归还
//   - 全局仅一个 SharedFramePool 实例, 由 server 持有, 所有 session 共享
// ───────────────────────────────────────────────────────────────────────────
#pragma once
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <spdlog/spdlog.h>

class SharedFramePool {
public:
    static constexpr size_t MIN_FRAME_SIZE = 512;
    static constexpr size_t MAX_FRAME_SIZE = 65536;
    static constexpr int    NUM_CLASSES    = 8;
    static constexpr size_t CLASS_SIZES[NUM_CLASSES] = {
        512, 1024, 2048, 4096, 8192, 16384, 32768, 65536
    };

    SharedFramePool()
    {
        for (int i = 0; i < NUM_CLASSES; ++i) {
            classes_[i].frame_size = CLASS_SIZES[i];
            classes_[i].current_cap = kInitialCaps[i];
            classes_[i].allocated.store(0, std::memory_order_relaxed);
            classes_[i].freelist.store(0, std::memory_order_relaxed);
            expansion_[i].store(0, std::memory_order_relaxed);
        }
    }

    ~SharedFramePool()
    {
        for (int i = 0; i < NUM_CLASSES; ++i) {
            void* node = untag_ptr(classes_[i].freelist.load(std::memory_order_relaxed));
            while (node) {
                void* next = *static_cast<void**>(node);
                delete[] static_cast<char*>(node);
                node = next;
            }
        }
    }

    char* acquire(size_t total_size)
    {
        size_t frame = round_up_frame(total_size);
        int    ci    = class_index(frame);
        return acquire_from(ci);
    }

    void release(char* data, size_t frame_len)
    {
        size_t frame = round_up_frame(frame_len);
        int    ci    = class_index(frame);
        release_to(ci, data);
    }

    // 全局已申请总字节数 (跨所有档位)
    uint64_t total_bytes_allocated() const
    {
        uint64_t total = 0;
        for (int i = 0; i < NUM_CLASSES; ++i) {
            int alloc = classes_[i].allocated.load(std::memory_order_relaxed);
            total += static_cast<uint64_t>(alloc) * (sizeof(void*) + CLASS_SIZES[i]);
        }
        return total;
    }

    struct Metrics {
        int class_allocated[8];
        int class_cap[8];
        int expansion_count[8];
    };
    Metrics metrics() const
    {
        Metrics m{};
        for (int i = 0; i < NUM_CLASSES; ++i) {
            m.class_allocated[i]  = classes_[i].allocated.load(std::memory_order_relaxed);
            m.class_cap[i]        = classes_[i].current_cap;
            m.expansion_count[i]  = expansion_[i].load(std::memory_order_relaxed);
        }
        return m;
    }

private:
    static constexpr int kInitialCaps[NUM_CLASSES] = {
        256, 128, 64, 32, 16, 8, 4, 2
    };

    // ──── Tagged pointer (ABA-safe) ────────────────────
    //
    // x64 虚拟地址仅用位 [47:0]。位 [63:48] 用作 tag。
    // 每次 CAS 成功时 tag 递增，消除 ABA 窗口。
    //
    // Layout: [63:48 tag] [47:0 ptr]

    static constexpr uintptr_t kPtrMask = 0x0000FFFFFFFFFFFFULL;

    static void* untag_ptr(uintptr_t tagged)
    {
        return reinterpret_cast<void*>(tagged & kPtrMask);
    }

    static uint16_t untag_tag(uintptr_t tagged)
    {
        return static_cast<uint16_t>(tagged >> 48);
    }

    static uintptr_t make_tagged(void* ptr, uint16_t tag)
    {
        return (static_cast<uintptr_t>(tag) << 48) | (reinterpret_cast<uintptr_t>(ptr) & kPtrMask);
    }

    // ──── Per-class pool ───────────────────────────────

    struct ClassPool {
        size_t                  frame_size;
        int                     current_cap;
        std::atomic<int>        allocated;
        std::atomic<uintptr_t>  freelist;   // tagged pointer
    };

    ClassPool         classes_[NUM_CLASSES];
    std::atomic<int>  expansion_[NUM_CLASSES];

    // ──── 档位路由 ────────────────────────────────────

    static size_t round_up_frame(size_t need)
    {
        if (need <= 512)   return 512;
        if (need <= 1024)  return 1024;
        if (need <= 2048)  return 2048;
        if (need <= 4096)  return 4096;
        if (need <= 8192)  return 8192;
        if (need <= 16384) return 16384;
        if (need <= 32768) return 32768;
        return 65536;
    }

    static int class_index(size_t frame_size)
    {
        switch (frame_size) {
            case 512:   return 0;
            case 1024:  return 1;
            case 2048:  return 2;
            case 4096:  return 3;
            case 8192:  return 4;
            case 16384: return 5;
            case 32768: return 6;
            default:    return 7;
        }
    }

    // ──── ABA-safe Lock-free Treiber stack ─────────────
    //
    // 节点布局: [next_raw_ptr (8B)] [data[frame_size]]
    // next_raw_ptr 是不带 tag 的裸指针，仅在节点被独占持有时访问
    // freelist 存储带 tag 的指针，保护 CAS 不受 ABA 影响

    char* acquire_from(int ci)
    {
        auto& cp = classes_[ci];
        uintptr_t tagged = cp.freelist.load(std::memory_order_relaxed);
        while (true) {
            void* node = untag_ptr(tagged);
            // freelist 空 → 分配新节点
            if (!node) {
                break;
            }
            // 节点被我们"预定"(尚未 pop), 可安全读取其 next 指针
            void* next = *static_cast<void**>(node);
            uint16_t cur_tag = untag_tag(tagged);
            uintptr_t new_tagged = make_tagged(next, cur_tag + 1);
            if (cp.freelist.compare_exchange_weak(tagged, new_tagged, std::memory_order_acquire, std::memory_order_relaxed)) {
                // CAS 成功: node 已从 freelist 移除, tag 已递增
                return static_cast<char*>(node) + sizeof(void*);
            }
            // CAS 失败: tagged 已被更新为新值, 循环重试
        }
        // freelist 空 → 扩容/分配
        int alloc = cp.allocated.load(std::memory_order_relaxed);
        if (alloc >= cp.current_cap) {
            cp.current_cap *= 2;
            expansion_[ci].fetch_add(1, std::memory_order_relaxed);
            spdlog::info("SharedFramePool: class[{}B] cap expanded to {} (allocated={})" , CLASS_SIZES[ci], cp.current_cap, alloc);
        }
        size_t node_sz = sizeof(void*) + cp.frame_size;
        char* raw = new char[node_sz];
        cp.allocated.fetch_add(1, std::memory_order_relaxed);
        return raw + sizeof(void*);
    }

    void release_to(int ci, char* data)
    {
        char* node_raw = data - sizeof(void*);
        auto& cp = classes_[ci];
        uintptr_t tagged = cp.freelist.load(std::memory_order_relaxed);
        do {
            void*    old_head = untag_ptr(tagged);
            uint16_t old_tag  = untag_tag(tagged);
            // 将当前 freelist 头链接到本节点的 next
            *static_cast<void**>(static_cast<void*>(node_raw)) = old_head;
            uintptr_t new_tagged = make_tagged(node_raw, old_tag + 1);
            if (cp.freelist.compare_exchange_weak(tagged, new_tagged, std::memory_order_release, std::memory_order_relaxed)) {
                return;
            }
        } while (true);
    }
};
