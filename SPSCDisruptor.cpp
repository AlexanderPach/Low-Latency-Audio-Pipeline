#include "disruptor.h"
#include <cstddef>

template <typename T, std::size_t N>
class SPSCDisruptor {
	// Bitwise AND operation to compute modulo for power-of-2 sizes
    static_assert((N& (N - 1)) == 0, "N must be a power of 2");
    static_assert(N >= 2, "Buffer size must be at least 2");

public:
    SPSCDisruptor() = default;

    // Non-copyable, non-movable
    SPSCDisruptor(const SPSCDisruptor&) = delete;
    SPSCDisruptor& operator=(const SPSCDisruptor&) = delete;

    /// Called by the PRODUCER thread only.
    /// Returns false if the buffer is full.
    template <typename... Args>
    bool try_publish(Args&&... args) {
        const int64_t seq = write_cursor_.load(
            std::memory_order_relaxed);
        const int64_t read = cached_read_cursor_;

        // Full when write is exactly N ahead of read
        if (seq - read >= static_cast<int64_t>(N)) {
            // Refresh our cached copy of the read cursor
            cached_read_cursor_ =
                read_cursor_.load(std::memory_order_acquire);
            if (seq - cached_read_cursor_ >=
                static_cast<int64_t>(N)) {
                return false; // genuinely full
            }
        }

        // Construct the element directly in the slot
        const std::size_t idx = seq & kMask;
        buffer_[idx].~T();
        new (&buffer_[idx]) T(std::forward<Args>(args)...);

        // Publish: make the write visible to the consumer
        write_cursor_.store(seq + 1, std::memory_order_release);
        return true;
    }

    /// Called by the CONSUMER thread only.
    /// Returns std::nullopt if the buffer is empty.
    std::optional<T> try_consume() {
        const int64_t seq = read_cursor_.load(
            std::memory_order_relaxed);
        const int64_t written = cached_write_cursor_;

        // Empty when read has caught up to write
        if (seq >= written) {
            // Refresh our cached copy of the write cursor
            cached_write_cursor_ =
                write_cursor_.load(std::memory_order_acquire);
            if (seq >= cached_write_cursor_) {
                return std::nullopt; // genuinely empty
            }
        }

        // Read from the slot
        const std::size_t idx = seq & kMask;
        std::optional<T> result(std::move(buffer_[idx]));

        // Advance read cursor
        read_cursor_.store(seq + 1, std::memory_order_release);
        return result;
    }

private:
    static constexpr std::size_t kMask = N - 1;

    // Hot data: the ring buffer itself
    alignas(kCacheLineSize) std::array<T, N> buffer_{};

    // Producer's cache line
    alignas(kCacheLineSize) std::atomic<int64_t> write_cursor_{ 0 };
    int64_t cached_read_cursor_{ 0 };  // producer's local cache

    // Consumer's cache line
    alignas(kCacheLineSize) std::atomic<int64_t> read_cursor_{ 0 };
    int64_t cached_write_cursor_{ 0 }; // consumer's local cache
};