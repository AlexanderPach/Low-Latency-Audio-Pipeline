#include "benchmark_queues.h"

#include <algorithm>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <latch>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;

struct Options {
    std::size_t iterations = 500000;
    std::size_t latency_samples = 5000;
    std::size_t trials = 5;
    std::size_t warmups = 1;
    std::size_t timeout_seconds = 30;
    std::size_t capacity = 0; // 0 means both supported capacities.
    bool self_test_only = false;
};

struct Stamped {
    std::uint32_t sequence = 0;
    Clock::time_point sent{};
};

struct Result {
    std::size_t count = 0;
    std::int64_t elapsed_ns = 0;
    std::uint64_t publish_retries = 0;
    std::uint64_t consume_retries = 0;
    std::uint64_t ack_waits = 0;
    std::uint64_t checksum = 0;
    std::vector<std::int64_t> latency;
};

void require(bool condition, const char* message) {
    if (!condition) throw std::runtime_error(message);
}

std::uint64_t expected_checksum(std::size_t count) {
    return static_cast<std::uint64_t>(count) * (count - 1) / 2;
}

// The gate excludes thread creation. The release and both workers' initial
// scheduling are included, as are validation, retry/yield, and OS scheduling.
struct Gate {
    std::latch ready{2};
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    Clock::time_point begin{};
    Clock::time_point deadline{};

    bool arrive(std::stop_token token) {
        ready.count_down();
        while (!start.load(std::memory_order_acquire)) {
            // If creating the other thread throws, jthread destruction requests
            // stop and must not get stuck waiting for a gate never released.
            if (token.stop_requested()) return false;
            std::this_thread::yield();
        }
        return true;
    }

    void release(std::size_t timeout_seconds) {
        ready.wait();
        begin = Clock::now();
        deadline = begin + std::chrono::seconds(timeout_seconds);
        start.store(true, std::memory_order_release);
    }

    bool cancelled(std::uint64_t waits) {
        if (stop.load(std::memory_order_relaxed)) return true;
        // Timeout clocks are only taken on every 256th failed attempt/wait;
        // there is no timestamp per payload in either throughput benchmark.
        if ((waits & 255) == 0 && Clock::now() >= deadline) {
            stop.store(true, std::memory_order_relaxed);
            return true;
        }
        return false;
    }
};

template <template <typename, std::size_t> class Queue, std::size_t Capacity>
Result serial_pair(const Options& options) {
    auto queue = std::make_unique<Queue<float, Capacity>>();
    Result result;
    result.count = options.iterations;
    const auto begin = Clock::now();
    const auto deadline = begin + std::chrono::seconds(options.timeout_seconds);
    for (std::size_t i = 0; i < options.iterations; ++i) {
        require(queue->try_publish(static_cast<float>(i)), "serial publication failed");
        const auto value = queue->try_consume();
        require(value.has_value() && *value == static_cast<float>(i), "serial FIFO validation failed");
        result.checksum += static_cast<std::uint64_t>(*value);
        if ((i & 4095) == 0) require(Clock::now() < deadline, "serial benchmark timed out");
    }
    result.elapsed_ns = std::chrono::duration_cast<Nanoseconds>(Clock::now() - begin).count();
    require(!queue->try_consume().has_value(), "serial queue not empty after expected count");
    require(result.checksum == expected_checksum(result.count), "serial checksum mismatch");
    return result;
}

template <template <typename, std::size_t> class Queue, std::size_t Capacity>
Result saturated_transfer(const Options& options) {
    auto queue = std::make_unique<Queue<float, Capacity>>();
    Gate gate;
    Result result;
    std::size_t published = 0;
    std::size_t consumed = 0;
    bool corrupt = false;
    Clock::time_point end{};

    std::jthread producer([&](std::stop_token token) {
        if (!gate.arrive(token)) return;
        std::uint64_t retries = 0;
        for (std::size_t i = 0; i < options.iterations; ++i) {
            while (!queue->try_publish(static_cast<float>(i))) {
                ++retries;
                if (gate.cancelled(retries)) return;
                std::this_thread::yield();
            }
        }
        published = options.iterations;
        result.publish_retries = retries;
    });
    std::jthread consumer([&](std::stop_token token) {
        if (!gate.arrive(token)) return;
        std::uint64_t retries = 0;
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < options.iterations; ++i) {
            std::optional<float> value;
            while (!(value = queue->try_consume()).has_value()) {
                ++retries;
                if (gate.cancelled(retries)) return;
                std::this_thread::yield();
            }
            if (*value != static_cast<float>(i)) {
                corrupt = true;
                gate.stop.store(true, std::memory_order_relaxed);
                return;
            }
            checksum += static_cast<std::uint64_t>(*value);
        }
        end = Clock::now();
        consumed = options.iterations;
        result.consume_retries = retries;
        result.checksum = checksum;
    });
    gate.release(options.timeout_seconds);
    producer.join();
    consumer.join();

    require(!corrupt, "saturated FIFO validation failed");
    require(!gate.stop.load(), "saturated benchmark timed out");
    require(end <= gate.deadline, "saturated benchmark exceeded its deadline");
    require(published == options.iterations && consumed == options.iterations,
            "saturated transfer count mismatch");
    require(!queue->try_consume().has_value(), "saturated queue not empty after expected count");
    require(result.checksum == expected_checksum(consumed), "saturated checksum mismatch");
    result.count = consumed;
    result.elapsed_ns = std::chrono::duration_cast<Nanoseconds>(end - gate.begin).count();
    return result;
}

template <template <typename, std::size_t> class Queue, std::size_t Capacity>
Result sparse_handoff(const Options& options) {
    auto queue = std::make_unique<Queue<Stamped, Capacity>>();
    Gate gate;
    std::atomic<std::size_t> acknowledged{0};
    Result result;
    // Allocate and touch all result storage before the gate/timed region.
    result.latency.resize(options.latency_samples);
    std::size_t published = 0;
    std::size_t consumed = 0;
    bool corrupt = false;
    Clock::time_point end{};

    std::jthread producer([&](std::stop_token token) {
        if (!gate.arrive(token)) return;
        std::uint64_t waits = 0;
        std::uint64_t retries = 0;
        for (std::size_t i = 0; i < options.latency_samples; ++i) {
            while (acknowledged.load(std::memory_order_acquire) != i) {
                ++waits;
                if (gate.cancelled(waits)) return;
                std::this_thread::yield();
            }
            // One item in flight, with its timestamp before publication. This
            // includes publish cost and polling/scheduling, not the ack return.
            const Stamped value{static_cast<std::uint32_t>(i), Clock::now()};
            while (!queue->try_publish(value)) {
                ++retries;
                if (gate.cancelled(retries)) return;
                std::this_thread::yield();
            }
        }
        published = options.latency_samples;
        result.ack_waits = waits;
        result.publish_retries = retries;
    });
    std::jthread consumer([&](std::stop_token token) {
        if (!gate.arrive(token)) return;
        std::uint64_t retries = 0;
        std::uint64_t checksum = 0;
        for (std::size_t i = 0; i < options.latency_samples; ++i) {
            std::optional<Stamped> value;
            while (!(value = queue->try_consume()).has_value()) {
                ++retries;
                if (gate.cancelled(retries)) return;
                std::this_thread::yield();
            }
            const auto received = Clock::now();
            if (value->sequence != i || received < value->sent) {
                corrupt = true;
                gate.stop.store(true, std::memory_order_relaxed);
                return;
            }
            result.latency[i] = std::chrono::duration_cast<Nanoseconds>(received - value->sent).count();
            checksum += value->sequence;
            // The release is after capture and storage: ack affects the next
            // handoff's cadence but is outside this item's measured interval.
            acknowledged.store(i + 1, std::memory_order_release);
        }
        end = Clock::now();
        consumed = options.latency_samples;
        result.consume_retries = retries;
        result.checksum = checksum;
    });
    gate.release(options.timeout_seconds);
    producer.join();
    consumer.join();

    require(!corrupt, "handoff FIFO/timestamp validation failed");
    require(!gate.stop.load(), "handoff benchmark timed out");
    require(end <= gate.deadline, "handoff benchmark exceeded its deadline");
    require(published == options.latency_samples && consumed == options.latency_samples,
            "handoff transfer count mismatch");
    require(!queue->try_consume().has_value(), "handoff queue not empty after expected count");
    require(result.checksum == expected_checksum(consumed), "handoff checksum mismatch");
    require(result.publish_retries == 0, "one-in-flight handoff unexpectedly encountered a full queue");
    result.count = consumed;
    result.elapsed_ns = std::chrono::duration_cast<Nanoseconds>(end - gate.begin).count();
    // Sorting is deliberately outside the measured interval.
    std::sort(result.latency.begin(), result.latency.end());
    return result;
}

template <template <typename, std::size_t> class Queue, std::size_t Capacity>
void test_queue() {
    auto queue = std::make_unique<Queue<float, Capacity>>();
    require(!queue->try_consume().has_value(), "self-test: new queue not empty");
    for (std::size_t cycle = 0; cycle < 16; ++cycle) {
        for (std::size_t i = 0; i < Capacity; ++i) {
            require(queue->try_publish(static_cast<float>(cycle * Capacity + i)), "self-test: capacity too small");
        }
        require(!queue->try_publish(-1.0f), "self-test: queue exceeded capacity");
        for (std::size_t i = 0; i < Capacity; ++i) {
            const auto value = queue->try_consume();
            require(value.has_value() && *value == static_cast<float>(cycle * Capacity + i),
                    "self-test: FIFO/full rejection/wraparound failure");
        }
        require(!queue->try_consume().has_value(), "self-test: drained queue not empty");
    }
}

void self_test() {
    test_queue<bench::SpscRing, 2>();
    test_queue<bench::MutexRing, 2>();
    test_queue<bench::SpscRing, 256>();
    test_queue<bench::MutexRing, 256>();
    test_queue<bench::SpscRing, 65536>();
    test_queue<bench::MutexRing, 65536>();
}

std::size_t parse_number(std::string_view value) {
    std::size_t result = 0;
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
    require(parsed.ec == std::errc{} && parsed.ptr == value.data() + value.size(), "invalid numeric option");
    return result;
}

Options parse_options(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--quick") {
            options.iterations = 20000;
            options.latency_samples = 1000;
            options.trials = 2;
            options.warmups = 1;
        } else if (argument == "--self-test") {
            options.self_test_only = true;
        } else if (argument == "--help") {
            std::cout << "Usage: queue_benchmark [--quick] [--self-test] [--iterations N]\n"
                         "  [--latency-samples N] [--trials N] [--warmups N]\n"
                         "  [--capacity 256|65536|all] [--timeout-seconds N]\n"
                         "CSV goes to stdout; metadata goes to stderr. Warmups are retained\n"
                         "as phase=warmup; use phase=measure for reported comparisons.\n";
            std::exit(0);
        } else {
            require(i + 1 < argc, "missing option value");
            const std::string_view value = argv[++i];
            if (argument == "--iterations") options.iterations = parse_number(value);
            else if (argument == "--latency-samples") options.latency_samples = parse_number(value);
            else if (argument == "--trials") options.trials = parse_number(value);
            else if (argument == "--warmups") options.warmups = parse_number(value);
            else if (argument == "--timeout-seconds") options.timeout_seconds = parse_number(value);
            else if (argument == "--capacity") options.capacity = value == "all" ? 0 : parse_number(value);
            else throw std::runtime_error("unknown option: " + std::string(argument));
        }
    }
    require(options.iterations > 0 && options.iterations <= 16777216,
            "iterations must be in 1..16777216 so float sequence IDs are exact");
    require(options.latency_samples > 0 && options.latency_samples <= 1000000,
            "latency-samples must be in 1..1000000");
    require(options.trials > 0 && options.trials <= 100, "trials must be in 1..100");
    require(options.warmups <= 10, "warmups must be in 0..10");
    require(options.timeout_seconds > 0 && options.timeout_seconds <= 300,
            "timeout-seconds must be in 1..300");
    require(options.capacity == 0 || options.capacity == 256 || options.capacity == 65536,
            "capacity must be 256, 65536 or all");
    return options;
}

std::int64_t percentile(const std::vector<std::int64_t>& sorted, std::size_t percent) {
    // Nearest rank: element ceil(percent * n / 100), with ranks starting at one.
    return sorted[(percent * sorted.size() + 99) / 100 - 1];
}

void emit(std::string_view phase, std::size_t trial, std::size_t order,
          std::string_view mode, std::string_view queue, std::size_t capacity,
          const Result& result) {
    require(result.elapsed_ns > 0, "timer could not resolve benchmark duration");
    const bool handoff = mode == "sparse_handoff";
    std::cout << phase << ',' << trial << ',' << order << ',' << mode << ',' << queue << ','
              << capacity << ',' << (handoff ? sizeof(Stamped) : sizeof(float)) << ','
              << (mode == "serial_pair" ? "pair" : handoff ? "handoff" : "transfer") << ','
              << result.count << ',' << result.elapsed_ns << ','
              << std::fixed << std::setprecision(6)
              << static_cast<double>(result.count) * 1e9 / result.elapsed_ns << ','
              << static_cast<double>(result.elapsed_ns) / result.count << ','
              << result.publish_retries << ',' << result.consume_retries << ','
              << result.ack_waits << ',' << result.checksum << ',';
    if (handoff) {
        std::cout << percentile(result.latency, 50) << ',' << percentile(result.latency, 95)
                  << ',' << percentile(result.latency, 99) << ',' << result.latency.back();
    } else {
        std::cout << ",,,";
    }
    std::cout << '\n';
}

template <std::size_t Capacity>
void run_capacity(const Options& options, std::size_t capacity_order) {
    constexpr std::string_view modes[] = {"serial_pair", "saturated_transfer", "sparse_handoff"};
    for (std::size_t round = 0; round < options.warmups + options.trials; ++round) {
        const auto phase = round < options.warmups ? "warmup" : "measure";
        const auto trial = round < options.warmups ? round + 1 : round - options.warmups + 1;
        for (std::size_t mode = 0; mode < 3; ++mode) {
            for (std::size_t order = 0; order < 2; ++order) {
                const bool spsc = (round + mode + capacity_order + order) % 2 == 0;
                Result result;
                if (mode == 0) {
                    result = spsc ? serial_pair<bench::SpscRing, Capacity>(options)
                                  : serial_pair<bench::MutexRing, Capacity>(options);
                } else if (mode == 1) {
                    result = spsc ? saturated_transfer<bench::SpscRing, Capacity>(options)
                                  : saturated_transfer<bench::MutexRing, Capacity>(options);
                } else {
                    result = spsc ? sparse_handoff<bench::SpscRing, Capacity>(options)
                                  : sparse_handoff<bench::MutexRing, Capacity>(options);
                }
                emit(phase, trial, order + 1, modes[mode], spsc ? "spsc" : "mutex", Capacity, result);
            }
        }
    }
}

void metadata(const Options& options) {
    std::atomic<std::int64_t> cursor{0};
    std::vector<std::int64_t> clock_cost(10000);
    for (auto& elapsed : clock_cost) {
        const auto begin = Clock::now();
        elapsed = std::chrono::duration_cast<Nanoseconds>(Clock::now() - begin).count();
    }
    std::sort(clock_cost.begin(), clock_cost.end());
    std::cerr << "iterations=" << options.iterations << '\n'
              << "latency_samples=" << options.latency_samples << '\n'
              << "trials=" << options.trials << '\n'
              << "warmups=" << options.warmups << '\n'
              << "capacity=" << options.capacity << " (0=256+65536)\n"
              << "timeout_seconds_per_trial=" << options.timeout_seconds << '\n'
              << "hardware_concurrency=" << std::thread::hardware_concurrency() << '\n'
              << "float_payload_bytes=" << sizeof(float) << '\n'
              << "stamped_payload_bytes=" << sizeof(Stamped) << '\n'
              << "atomic_int64_is_always_lock_free=" << std::atomic<std::int64_t>::is_always_lock_free << '\n'
              << "atomic_int64_runtime_is_lock_free=" << cursor.is_lock_free() << '\n'
              << "clock_is_steady=" << Clock::is_steady << '\n'
              << "clock_period_seconds=" << Clock::period::num << '/' << Clock::period::den << '\n'
              << "back_to_back_clock_median_ns=" << percentile(clock_cost, 50) << '\n'
              << "back_to_back_clock_p99_ns=" << percentile(clock_cost, 99) << '\n'
              << "clock_overhead_subtracted=false\n"
              << "percentiles=nearest_rank; latency includes timestamp, enqueue, dequeue, polling and scheduling\n"
              << "retry_policy=yield; no CPU affinity or real-time scheduling requested\n"
              << "publish_retries=full returns; consume_retries=empty returns; ack_waits=failed ack polls\n"
              << "throughput_includes=FIFO validation, checksum, retry/yield and worker scheduling\n"
              << "throughput_timestamps=interval boundaries; serial timeout check every 4096 pairs; threaded timeout checks on waits\n"
              << "order=alternating candidates by round, mode and capacity\n";
#if defined(__VERSION__)
    std::cerr << "compiler=" << __VERSION__ << '\n';
#elif defined(_MSC_VER)
    std::cerr << "compiler_msvc=" << _MSC_VER << '\n';
#endif
#ifdef NDEBUG
    std::cerr << "NDEBUG=defined\n";
#else
    std::cerr << "NDEBUG=not_defined; use an optimized Release build for performance claims\n";
#endif
}

} // namespace

int main(int argc, char** argv) {
    try {
        const Options options = parse_options(argc, argv);
        self_test();
        std::cerr << "queue_self_test=passed\n";
        if (options.self_test_only) return 0;
        metadata(options);
        std::cout << "phase,trial,order,mode,queue,capacity,payload_bytes,unit,count,elapsed_ns,"
                     "units_per_second,ns_per_unit,publish_retries,consume_retries,ack_waits,"
                     "checksum,p50_ns,p95_ns,p99_ns,max_ns\n";
        if (options.capacity == 0 || options.capacity == 256) run_capacity<256>(options, 0);
        if (options.capacity == 0 || options.capacity == 65536) run_capacity<65536>(options, 1);
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "benchmark_failed=" << error.what() << '\n';
        return 1;
    }
}
