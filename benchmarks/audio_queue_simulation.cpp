#include "benchmark_queues.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <charconv>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <exception>
#include <iomanip>
#include <iostream>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;
using Nanoseconds = std::chrono::nanoseconds;
constexpr std::uint64_t sample_rate = 44100;
constexpr std::uint64_t block_frames = 256;

struct Options {
    unsigned trials = 3;
    std::uint64_t warmup_ms = 2000;
    std::uint64_t measure_ms = 3000;
    std::size_t capacity = 0; // Zero selects both supported capacities.
    bool self_test = false;
};

std::uint64_t number(std::string_view value, std::string_view name) {
    std::uint64_t result = 0;
    const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), result);
    if (error != std::errc{} || end != value.data() + value.size()) {
        throw std::runtime_error("Invalid numeric value for " + std::string(name));
    }
    return result;
}

void help() {
    std::cout
        << "Usage: audio_queue_simulation [options]\n"
        << "  --trials N          Repeated trials per candidate (default 3)\n"
        << "  --warmup-ms N       Excluded warmup (default 2000 ms)\n"
        << "  --measure-ms N      Measured interval (default 3000 ms)\n"
        << "  --capacity N|all    256, 65536, or all (default all)\n"
        << "  --quick             Short correctness/smoke run, not steady-state data\n"
        << "  --self-test         Short validated run at capacity 256\n"
        << "  --help              Show this help\n\n"
        << "Writes per-trial CSV to stdout. Diagnostics go to stderr.\n"
        << "This is a paced queue simulation, not an audio-device measurement.\n";
}

Options parse(int argc, char** argv) {
    Options options;
    for (int i = 1; i < argc; ++i) {
        const std::string_view argument = argv[i];
        if (argument == "--help") {
            help();
            std::exit(0);
        }
        if (argument == "--quick" || argument == "--self-test") {
            options.trials = 1;
            options.warmup_ms = argument == "--self-test" ? 6 : 20;
            options.measure_ms = argument == "--self-test" ? 18 : 50;
            options.self_test = argument == "--self-test";
            if (options.self_test) options.capacity = 256;
            continue;
        }
        if (i + 1 >= argc) throw std::runtime_error("Missing value for " + std::string(argument));
        const std::string_view value = argv[++i];
        if (argument == "--trials") {
            const auto parsed = number(value, argument);
            if (parsed == 0 || parsed > 100) throw std::runtime_error("Trials must be 1..100");
            options.trials = static_cast<unsigned>(parsed);
        } else if (argument == "--warmup-ms") {
            options.warmup_ms = number(value, argument);
        } else if (argument == "--measure-ms") {
            options.measure_ms = number(value, argument);
        } else if (argument == "--capacity") {
            options.capacity = value == "all" ? 0 : static_cast<std::size_t>(number(value, argument));
            if (options.capacity != 0 && options.capacity != 256 && options.capacity != 65536) {
                throw std::runtime_error("Capacity must be 256, 65536, or all");
            }
        } else {
            throw std::runtime_error("Unknown option: " + std::string(argument));
        }
    }
    if (options.warmup_ms > 60000 || options.measure_ms == 0 || options.measure_ms > 60000) {
        throw std::runtime_error("Warmup must be 0..60000 ms and measurement 1..60000 ms");
    }
    return options;
}

std::int64_t now_ns() {
    return std::chrono::duration_cast<Nanoseconds>(Clock::now().time_since_epoch()).count();
}

std::uint64_t blocks_for_ms(std::uint64_t milliseconds) {
    constexpr auto divisor = block_frames * 1000;
    return (milliseconds * sample_rate + divisor - 1) / divisor;
}

Nanoseconds block_offset(std::uint64_t blocks) {
    // Compute from the absolute block index so rounding does not accumulate.
    return Nanoseconds(blocks * block_frames * 1000000000ULL / sample_rate);
}

struct Quantiles {
    std::int64_t p50 = 0;
    std::int64_t p95 = 0;
    std::int64_t p99 = 0;
    std::int64_t maximum = 0;
};

Quantiles quantiles(std::vector<std::int64_t>& values) {
    if (values.empty()) return {};
    std::sort(values.begin(), values.end());
    const auto at = [&](std::size_t percent) {
        // Nearest rank: ceil(percent / 100 * count), using one-based ranks.
        return values[(values.size() * percent + 99) / 100 - 1];
    };
    return {at(50), at(95), at(99), values.back()};
}

struct Result {
    std::uint64_t warmup_blocks = 0;
    std::uint64_t measured_blocks = 0;
    std::uint64_t consumed = 0;
    std::uint64_t underruns = 0;
    std::uint64_t callbacks_with_underrun = 0;
    std::uint64_t callback_over_budget = 0;
    std::uint64_t missed_next_deadline = 0;
    std::uint64_t published_total = 0;
    std::uint64_t consumed_total = 0;
    std::uint64_t final_occupancy = 0;
    std::uint64_t full_retries_total = 0;
    std::uint64_t checksum = 0;
    Quantiles sample_age;
    Quantiles callback_duration;
    Quantiles wake_lateness;
};

template <template <typename, std::size_t> class Queue, std::size_t Capacity>
Result simulate(const Options& options) {
    Result result;
    result.warmup_blocks = blocks_for_ms(options.warmup_ms);
    result.measured_blocks = blocks_for_ms(options.measure_ms);
    const auto total_blocks = result.warmup_blocks + result.measured_blocks;
    const auto scheduled_frames = total_blocks * block_frames;
    const auto max_sequence = scheduled_frames + Capacity + block_frames;
    if (max_sequence >= (1ULL << 24)) {
        throw std::runtime_error("Sequence IDs must remain exactly representable in float");
    }

    // Each sequence owns one timestamp for the entire run. The producer writes
    // it before publishing its float ID; the queue's synchronization publishes
    // that metadata to the consumer. No slot is reused or overwritten while read.
    std::vector<std::int64_t> generated(max_sequence);
    std::vector<std::int64_t> ages(result.measured_blocks * block_frames);
    std::vector<std::int64_t> callbacks(result.measured_blocks);
    std::vector<std::int64_t> lateness(result.measured_blocks);
    std::array<std::int64_t, block_frames> block_generated{};
    auto queue = std::make_unique<Queue<float, Capacity>>();
    std::atomic<bool> prefilled{false};
    std::atomic<bool> producer_failed{false};
    std::exception_ptr producer_error;

    std::jthread producer([&](std::stop_token stop) {
        try {
            std::uint64_t sequence = 0;
            std::uint64_t full_retries = 0;
            while (!stop.stop_requested()) {
                if (sequence >= max_sequence) {
                    throw std::runtime_error("Producer exceeded the preallocated timestamp bound");
                }
                // Timestamp once, before the first enqueue attempt. Thus age
                // includes waiting for a full queue, like the app's eager producer.
                generated[sequence] = now_ns();
                while (!queue->try_publish(static_cast<float>(sequence))) {
                    ++full_retries;
                    if (stop.stop_requested()) {
                        result.published_total = sequence;
                        result.full_retries_total = full_retries;
                        return;
                    }
                    std::this_thread::yield();
                }
                ++sequence;
                if (sequence == Capacity) prefilled.store(true, std::memory_order_release);
            }
            result.published_total = sequence;
            result.full_retries_total = full_retries;
        } catch (...) {
            producer_error = std::current_exception();
            producer_failed.store(true, std::memory_order_release);
        }
    });

    const auto prefill_deadline = Clock::now() + std::chrono::seconds(5);
    while (!prefilled.load(std::memory_order_acquire)) {
        if (producer_failed.load(std::memory_order_acquire)) {
            producer.request_stop();
            producer.join();
            std::rethrow_exception(producer_error);
        }
        if (Clock::now() > prefill_deadline) throw std::runtime_error("Timed out prefilling queue");
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    const auto begin = Clock::now();
    const auto final_deadline = begin + block_offset(total_blocks + 1) + std::chrono::seconds(5);
    std::uint64_t expected_sequence = 0;
    std::uint64_t measured_samples = 0;
    const auto period = block_offset(1);
    for (std::uint64_t block = 0; block < total_blocks; ++block) {
        const auto scheduled = begin + block_offset(block + 1);
        const auto next_scheduled = begin + block_offset(block + 2);
        // Overdue callbacks are executed in order, never silently skipped.
        std::this_thread::sleep_until(scheduled);
        const auto callback_start = Clock::now();
        if (callback_start > final_deadline) throw std::runtime_error("Simulation exceeded wall-clock bound");
        if (producer_failed.load(std::memory_order_acquire)) {
            producer.request_stop();
            producer.join();
            std::rethrow_exception(producer_error);
        }
        std::size_t received = 0;
        std::uint64_t underruns = 0;
        bool corrupted = false;
        for (std::uint64_t frame = 0; frame < block_frames; ++frame) {
            if (auto sample = queue->try_consume()) {
                if (*sample != static_cast<float>(expected_sequence)) {
                    corrupted = true;
                    break;
                }
                block_generated[received++] = generated[expected_sequence];
                result.checksum += expected_sequence++;
            } else {
                ++underruns;
            }
        }
        const auto callback_end = Clock::now();
        if (corrupted) throw std::runtime_error("Queue lost, duplicated, or reordered a sample");

        // Recording/quantile storage is outside the callback duration interval.
        if (block >= result.warmup_blocks) {
            const auto index = block - result.warmup_blocks;
            callbacks[index] = std::chrono::duration_cast<Nanoseconds>(callback_end - callback_start).count();
            lateness[index] = std::max<std::int64_t>(0,
                std::chrono::duration_cast<Nanoseconds>(callback_start - scheduled).count());
            if (callback_end - callback_start > period) ++result.callback_over_budget;
            if (callback_end > next_scheduled) ++result.missed_next_deadline;
            result.consumed += received;
            result.underruns += underruns;
            if (underruns != 0) ++result.callbacks_with_underrun;
            const auto delivered_ns = std::chrono::duration_cast<Nanoseconds>(callback_end.time_since_epoch()).count();
            for (std::size_t sample = 0; sample < received; ++sample) {
                const auto age = delivered_ns - block_generated[sample];
                if (age < 0) throw std::runtime_error("Negative generated-to-callback age");
                ages[measured_samples++] = age;
            }
        }
    }
    producer.request_stop();
    producer.join();
    if (producer_error) std::rethrow_exception(producer_error);
    if (result.consumed + result.underruns != result.measured_blocks * block_frames) {
        throw std::runtime_error("Consumed/underrun counts do not match scheduled frames");
    }
    if (result.checksum != expected_sequence * (expected_sequence - 1) / 2) {
        throw std::runtime_error("Consumed sequence checksum mismatch");
    }
    if (result.published_total < expected_sequence ||
        result.published_total - expected_sequence > Capacity) {
        throw std::runtime_error("Final queue occupancy is out of bounds");
    }
    result.consumed_total = expected_sequence;
    result.final_occupancy = result.published_total - expected_sequence;
    if (measured_samples == 0) throw std::runtime_error("No samples were consumed during measurement");
    ages.resize(measured_samples);
    result.sample_age = quantiles(ages);
    result.callback_duration = quantiles(callbacks);
    result.wake_lateness = quantiles(lateness);
    return result;
}

void print_header() {
    std::cout
        << "trial,execution_order,queue,capacity,sample_rate,block_frames,warmup_callbacks,measured_callbacks,"
        << "warmup_scheduled_seconds,measured_scheduled_seconds,consumed_samples,underrun_samples,callbacks_with_underrun,"
        << "callback_over_budget,missed_next_deadline,published_samples_total,consumed_samples_total,"
        << "final_queue_occupancy,producer_full_retries_total,"
        << "checksum_all_consumed,age_p50_ns,age_p95_ns,age_p99_ns,age_max_ns,callback_p50_ns,"
        << "callback_p95_ns,callback_p99_ns,callback_max_ns,wake_lateness_p99_ns,wake_lateness_max_ns\n";
}

void print(const Result& r, unsigned trial, unsigned order, const char* queue, std::size_t capacity) {
    std::cout << std::setprecision(12) << trial << ',' << order << ',' << queue << ',' << capacity << ','
        << sample_rate << ',' << block_frames << ',' << r.warmup_blocks << ',' << r.measured_blocks << ','
        << block_offset(r.warmup_blocks).count() / 1e9 << ','
        << block_offset(r.measured_blocks).count() / 1e9 << ','
        << r.consumed << ',' << r.underruns << ',' << r.callbacks_with_underrun << ','
        << r.callback_over_budget << ',' << r.missed_next_deadline << ',' << r.published_total << ','
        << r.consumed_total << ',' << r.final_occupancy << ',' << r.full_retries_total << ',' << r.checksum << ','
        << r.sample_age.p50 << ',' << r.sample_age.p95 << ',' << r.sample_age.p99 << ',' << r.sample_age.maximum << ','
        << r.callback_duration.p50 << ',' << r.callback_duration.p95 << ',' << r.callback_duration.p99 << ','
        << r.callback_duration.maximum << ',' << r.wake_lateness.p99 << ',' << r.wake_lateness.maximum << '\n';
    if (!std::cout) throw std::runtime_error("Failed writing CSV output");
}

template <std::size_t Capacity>
void run_capacity(const Options& options, unsigned trial, unsigned& order, bool mutex_first) {
    for (unsigned candidate = 0; candidate < 2; ++candidate) {
        const bool mutex = (candidate == 0) == mutex_first;
        std::cerr << "trial=" << trial << " order=" << order << " capacity=" << Capacity
                  << " queue=" << (mutex ? "mutex" : "spsc") << '\n';
        if (mutex) {
            print(simulate<bench::MutexRing, Capacity>(options), trial, order++, "mutex", Capacity);
        } else {
            print(simulate<bench::SpscRing, Capacity>(options), trial, order++, "spsc", Capacity);
        }
    }
}

} // namespace

int main(int argc, char** argv) {
    try {
        const auto options = parse(argc, argv);
        std::cerr << "Paced queue simulation only; no audio device, sine generation, or hardware latency.\n"
                  << "Float sequence payload; generation timestamps use a preallocated side array.\n"
                  << "Age includes full-queue waiting and ends at simulated callback completion.\n"
                  << "Callback timing includes sequence checking and timestamp-copy instrumentation.\n"
                  << "Nearest-rank quantiles; no CPU pinning; eager producer yields on full.\n"
                  << "float_bytes=" << sizeof(float) << " timestamp_bytes=" << sizeof(std::int64_t)
                  << " int64_atomics_always_lock_free=" << std::atomic<std::int64_t>::is_always_lock_free << '\n';
        if (options.warmup_ms < 1487 && (options.capacity == 0 || options.capacity == 65536)) {
            std::cerr << "WARNING: short warmup does not establish steady state for the 65536-slot queue.\n";
        }
        print_header();
        for (unsigned trial = 1; trial <= options.trials; ++trial) {
            unsigned order = 0;
            const bool reverse_capacity = trial % 2 == 0;
            for (unsigned position = 0; position < 2; ++position) {
                const auto capacity = ((position == 0) != reverse_capacity) ? 256U : 65536U;
                if (options.capacity != 0 && options.capacity != capacity) continue;
                // Alternate candidate order within each capacity, independently
                // of the reversed capacity order on alternating trials.
                const bool mutex_first = (trial + (capacity == 65536 ? 1U : 0U)) % 2 == 0;
                if (capacity == 256) run_capacity<256>(options, trial, order, mutex_first);
                else run_capacity<65536>(options, trial, order, mutex_first);
            }
        }
        if (options.self_test) std::cerr << "Simulation self-test passed.\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "audio_queue_simulation: " << error.what() << '\n';
        return 1;
    }
}
