#pragma once

#include <array>
#include <cstddef>
#include <mutex>
#include <optional>
#include <utility>

#include "SPSCDisruptor.cpp"

namespace bench {

template <typename T, std::size_t N>
using SpscRing = ::SPSCDisruptor<T, N>;

// Same bounded, preallocated FIFO and full/empty return values as SpscRing.
// This deliberately uses lock_guard, rather than try_lock: lock contention may
// delay a call even though queue-full/queue-empty never waits for data or space.
template <typename T, std::size_t N>
class MutexRing {
    static_assert(N >= 2 && (N & (N - 1)) == 0);

public:
    template <typename... Args>
    bool try_publish(Args&&... args) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == N) return false;
        buffer_[write_] = T(std::forward<Args>(args)...);
        write_ = (write_ + 1) & (N - 1);
        ++size_;
        return true;
    }

    std::optional<T> try_consume() {
        std::lock_guard<std::mutex> lock(mutex_);
        if (size_ == 0) return std::nullopt;
        std::optional<T> result(std::move(buffer_[read_]));
        read_ = (read_ + 1) & (N - 1);
        --size_;
        return result;
    }

private:
    std::array<T, N> buffer_{};
    std::size_t read_ = 0;
    std::size_t write_ = 0;
    std::size_t size_ = 0;
    std::mutex mutex_;
};

} // namespace bench
