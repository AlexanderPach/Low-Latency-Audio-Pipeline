#include "test_support.h"
#include "audio_engine.h"

#include <chrono>
#include <thread>

namespace {

void fill_queue(AudioEngine::AudioQueue& queue) {
    for (std::size_t i = 0; i < AudioEngine::kBufferSize; ++i) {
        REQUIRE(queue.try_publish(0.25f));
    }
    REQUIRE(!queue.try_publish(-0.25f));
}

void wait_for_consumption(AudioEngine::AudioQueue& queue) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    // start() may prime the device synchronously. Refill those freed slots so
    // success below requires consumption after start() has returned.
    while (queue.try_publish(-0.25f)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
    }
    // Only the miniaudio callback thread can now free a slot in the full queue.
    while (!queue.try_publish(-0.25f)) {
        REQUIRE(std::chrono::steady_clock::now() < deadline);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
}

void drain_queue(AudioEngine::AudioQueue& queue) {
    // Call only while stopped: the test and audio callback must not both consume.
    while (queue.try_consume().has_value()) {
    }
}

void consumes_samples() {
    AudioEngine engine;
    fill_queue(engine.get_queue());
    engine.start();
    wait_for_consumption(engine.get_queue());
    engine.stop();
}

void restart() {
    AudioEngine engine;
    for (int cycle = 0; cycle < 3; ++cycle) {
        fill_queue(engine.get_queue());
        engine.start();
        wait_for_consumption(engine.get_queue());
        engine.stop();

        drain_queue(engine.get_queue());
        fill_queue(engine.get_queue());
        // A stopped device must leave queued samples alone, even across periods.
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        REQUIRE(!engine.get_queue().try_publish(-0.25f));
        drain_queue(engine.get_queue());
    }
}

void destruction_running() {
    // Destruction must join the audio thread before destroying its queue.
    for (int cycle = 0; cycle < 3; ++cycle) {
        AudioEngine engine;
        fill_queue(engine.get_queue());
        engine.start();
        wait_for_consumption(engine.get_queue());
    }
}

} // namespace

int main(int argc, char** argv) {
    return test::run(argc, argv, {
        {"consumes_samples", consumes_samples},
        {"restart", restart},
        {"destruction_running", destruction_running},
    });
}
