#pragma once
#include "includes.hpp"

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <utility>

template <class T, size_t CapacityPow2>
class SpscRing {
    static_assert(CapacityPow2 >= 2, "Capacity too small");
    static_assert((CapacityPow2 & (CapacityPow2 - 1)) == 0, "Capacity must be power of two");

public:
    bool try_push(const T& v) { return try_push_impl(v); }
    bool try_push(T&& v) { return try_push_impl(std::move(v)); }

    bool try_pop(T& out) {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head == tail) return false;

        out = std::move(m_buf[head & kMask]);
        m_head.store(head + 1, std::memory_order_release);
        return true;
    }

    bool try_peek_ptr(const T*& outPtr) const {
        const size_t head = m_head.load(std::memory_order_relaxed);
        const size_t tail = m_tail.load(std::memory_order_acquire);
        if (head == tail) return false;
        outPtr = &m_buf[head & kMask];
        return true;
    }

    void clear() {
        const size_t tail = m_tail.load(std::memory_order_acquire);
        m_head.store(tail, std::memory_order_release);
    }

    uint64_t dropped() const { return m_dropped.load(std::memory_order_relaxed); }

private:
    static constexpr size_t kMask = CapacityPow2 - 1;

    template <class U>
    bool try_push_impl(U&& v) {
        const size_t tail = m_tail.load(std::memory_order_relaxed);
        const size_t head = m_head.load(std::memory_order_acquire);
        const size_t next = tail + 1;

        if ((next - head) > CapacityPow2) {
            m_dropped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        m_buf[tail & kMask] = std::forward<U>(v);
        m_tail.store(next, std::memory_order_release);
        return true;
    }

    alignas(64) mutable std::atomic<size_t> m_head{0};
    alignas(64) std::atomic<size_t> m_tail{0};
    std::array<T, CapacityPow2> m_buf{};
    std::atomic<uint64_t> m_dropped{0};
};

constexpr size_t INPUT_RING_CAP = 1u << 16;

inline SpscRing<InputEvent, INPUT_RING_CAP> g_inputRing;