#include "test_support.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <deque>
#include <memory>
#include <thread>
#include <utility>

#include "SPSCDisruptor.cpp"

namespace {

void empty() {
    SPSCDisruptor<int, 2> queue;
    REQUIRE(!queue.try_consume().has_value());
    REQUIRE(!queue.try_consume().has_value());

    // Empty reads must not prevent either initial publication or later reuse.
    for (int value = 0; value < 16; ++value) {
        REQUIRE(queue.try_publish(value));
        REQUIRE(queue.try_consume() == value);
        REQUIRE(!queue.try_consume().has_value());
    }
}

void fifo() {
    SPSCDisruptor<int, 8> queue;
    const std::array values{17, -3, 0, 92, -100};
    for (int value : values) {
        REQUIRE(queue.try_publish(value));
    }
    for (int value : values) {
        REQUIRE(queue.try_consume() == value);
    }
    REQUIRE(!queue.try_consume().has_value());
}

template <std::size_t Capacity>
void check_capacity() {
    SPSCDisruptor<int, Capacity> queue;
    for (std::size_t i = 0; i < Capacity; ++i) {
        REQUIRE(queue.try_publish(static_cast<int>(i)));
    }
    REQUIRE(!queue.try_publish(-1));
    REQUIRE(!queue.try_publish(-2));

    // A failed publication must neither overwrite unread data nor advance FIFO.
    REQUIRE(queue.try_consume() == 0);
    REQUIRE(queue.try_publish(static_cast<int>(Capacity)));
    REQUIRE(!queue.try_publish(-3));
    for (std::size_t i = 1; i <= Capacity; ++i) {
        REQUIRE(queue.try_consume() == static_cast<int>(i));
    }
    REQUIRE(!queue.try_consume().has_value());
}

void capacity() {
    check_capacity<2>();
    check_capacity<8>();
}

void wraparound() {
    SPSCDisruptor<int, 8> queue;
    std::deque<int> reference;
    std::uint32_t random = 0x12345678;
    int next_value = 0;

    // A fixed seed keeps the mixture of partial drains, fills and wraps repeatable.
    for (int operation = 0; operation < 50000; ++operation) {
        random = random * 1664525u + 1013904223u;
        if ((random >> 16) % 2 == 0) {
            const bool expected = reference.size() < 8;
            REQUIRE(queue.try_publish(next_value) == expected);
            if (expected) {
                reference.push_back(next_value++);
            }
        } else {
            auto value = queue.try_consume();
            REQUIRE(value.has_value() == !reference.empty());
            if (value) {
                REQUIRE(*value == reference.front());
                reference.pop_front();
            }
        }
    }
    while (!reference.empty()) {
        REQUIRE(queue.try_consume() == reference.front());
        reference.pop_front();
    }
    REQUIRE(!queue.try_consume().has_value());
}

void move_only() {
    SPSCDisruptor<std::unique_ptr<int>, 2> queue;
    auto first = std::make_unique<int>(11);
    auto second = std::make_unique<int>(22);
    auto third = std::make_unique<int>(33);
    REQUIRE(queue.try_publish(std::move(first)));
    REQUIRE(!first);
    REQUIRE(queue.try_publish(std::move(second)));
    REQUIRE(!second);
    REQUIRE(!queue.try_publish(std::move(third)));
    REQUIRE(third && *third == 33);

    auto consumed = queue.try_consume();
    REQUIRE(consumed.has_value() && *consumed && **consumed == 11);
    REQUIRE(queue.try_publish(std::move(third)));
    REQUIRE(!third);
    consumed = queue.try_consume();
    REQUIRE(consumed.has_value() && *consumed && **consumed == 22);
    consumed = queue.try_consume();
    REQUIRE(consumed.has_value() && *consumed && **consumed == 33);
    REQUIRE(!queue.try_consume().has_value());
}

struct Resource {
    static inline int alive = 0;
    int value;

    explicit Resource(int value) : value(value) { ++alive; }
    ~Resource() { --alive; }
};

struct TrackedValue {
    static inline int alive = 0;
    static inline int constructed = 0;
    static inline int destroyed = 0;
    std::unique_ptr<Resource> resource;

    TrackedValue() { ++alive; ++constructed; }
    TrackedValue(int a, int b) : resource(std::make_unique<Resource>(a + b)) {
        ++alive;
        ++constructed;
    }
    TrackedValue(TrackedValue&& other) noexcept : resource(std::move(other.resource)) {
        ++alive;
        ++constructed;
    }
    TrackedValue(const TrackedValue&) = delete;
    TrackedValue& operator=(const TrackedValue&) = delete;
    ~TrackedValue() { --alive; ++destroyed; }
};

void lifetime() {
    REQUIRE(TrackedValue::alive == 0);
    REQUIRE(Resource::alive == 0);
    TrackedValue::constructed = TrackedValue::destroyed = 0;
    {
        SPSCDisruptor<TrackedValue, 2> queue;
        REQUIRE(TrackedValue::alive == 2);
        for (int cycle = 0; cycle < 100; ++cycle) {
            REQUIRE(queue.try_publish(cycle, 7));
            REQUIRE(Resource::alive == 1);
            {
                auto value = queue.try_consume();
                REQUIRE(value.has_value());
                REQUIRE(value->resource && value->resource->value == cycle + 7);
                REQUIRE(TrackedValue::alive == 3);
                REQUIRE(Resource::alive == 1);
            }
            REQUIRE(TrackedValue::alive == 2);
            REQUIRE(Resource::alive == 0);
        }
        // Queue destruction must also destroy elements that were never consumed.
        REQUIRE(queue.try_publish(1, 2));
        REQUIRE(queue.try_publish(3, 4));
        REQUIRE(!queue.try_publish(5, 6));
        REQUIRE(Resource::alive == 2);
        REQUIRE(TrackedValue::alive == 2);
    }
    REQUIRE(Resource::alive == 0);
    REQUIRE(TrackedValue::alive == 0);
    REQUIRE(TrackedValue::constructed == TrackedValue::destroyed);
}

struct Message {
    std::uint64_t sequence;
    std::uint64_t inverse;
    std::array<std::uint64_t, 2> payload;
};

Message message_for(std::uint64_t sequence) {
    return {sequence, ~sequence, {sequence * 13, sequence ^ 0xa5a5a5a5a5a5a5a5ULL}};
}

bool matches(const Message& message, std::uint64_t sequence) {
    const auto expected = message_for(sequence);
    return message.sequence == expected.sequence && message.inverse == expected.inverse
        && message.payload == expected.payload;
}

template <std::size_t Capacity>
void check_concurrent(std::uint64_t count) {
    // Allocate on the heap so the production-capacity case also fits small stacks.
    auto queue = std::make_unique<SPSCDisruptor<Message, Capacity>>();
    std::atomic<bool> start{false};
    std::jthread producer([&](std::stop_token stop) {
        while (!start.load(std::memory_order_acquire) && !stop.stop_requested()) {
            std::this_thread::yield();
        }
        for (std::uint64_t sequence = 0; sequence < count && !stop.stop_requested(); ++sequence) {
            const auto message = message_for(sequence);
            while (!queue->try_publish(message)) {
                if (stop.stop_requested()) return;
                std::this_thread::yield();
            }
            if (sequence % 127 == 0) std::this_thread::yield();
        }
    });

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    start.store(true, std::memory_order_release);
    std::uint64_t received = 0;
    bool corrupted = false;
    while (received < count && std::chrono::steady_clock::now() < deadline) {
        if (auto message = queue->try_consume()) {
            if (!matches(*message, received)) {
                corrupted = true;
                break;
            }
            ++received;
            if (received % 61 == 0) std::this_thread::yield();
        } else {
            std::this_thread::yield();
        }
    }
    // Stop and join before throwing, including on timeout or corrupted data.
    producer.request_stop();
    producer.join();
    REQUIRE(!corrupted);
    REQUIRE(received == count);
    REQUIRE(!queue->try_consume().has_value());
}

void concurrent_small() { check_concurrent<2>(200000); }
void concurrent_audio() { check_concurrent<65536>(65536 * 8 + 1); }

} // namespace

int main(int argc, char** argv) {
    return test::run(argc, argv, {
        {"empty", empty},
        {"fifo", fifo},
        {"capacity", capacity},
        {"wraparound", wraparound},
        {"move_only", move_only},
        {"lifetime", lifetime},
        {"concurrent_small", concurrent_small},
        {"concurrent_audio", concurrent_audio},
    });
}
