// LLAP.cpp : Defines the entry point for the application.
// Low Latency Audio Pipeline

#include "LLAP.h"
#include "audio_engine.h"
#include <iostream>
#include <thread>
#include <cmath>
#include <atomic>
#include <numbers>

std::atomic<bool> g_running{ true };

void runAudioProducer(AudioEngine::AudioQueue& queue) {
    const double sampleRate = 44100.0;
    const double frequency = 440.0;
    const double angularVelocity = 2.0 * std::numbers::pi * frequency;

    uint64_t sampleIndex = 0;

    std::cout << "Producer thread launched. Generating 440Hz tone...\n";

    while (g_running.load(std::memory_order_relaxed)) {
        double time = sampleIndex / sampleRate;

        float sampleValue = static_cast<float>(std::sin(angularVelocity * time));

        sampleValue *= 0.1f;

        while (!queue.try_publish(sampleValue)) {
            if (!g_running.load(std::memory_order_relaxed)) return;
            std::this_thread::yield();
        }

        sampleIndex++;
    }

    std::cout << "Producer thread shutting down cleanly.\n";
}

int main() {
    try {
        std::cout << "===========================================\n";
        std::cout << " Lock-Free Audio Pipeline     \n";
        std::cout << "===========================================\n";

        AudioEngine audioEngine;

        std::thread producerThread(runAudioProducer, std::ref(audioEngine.get_queue()));

        audioEngine.start();
        std::cout << "\nAudio pipeline is LIVE. You should hear a 440Hz tone.\n";
        std::cout << "Press ENTER to stop the engine...\n\n";

        std::cin.get();

        std::cout << "Stopping audio pipelines...\n";
        audioEngine.stop();

        g_running.store(false, std::memory_order_relaxed); // Signal producer loop to terminate
        if (producerThread.joinable()) {
            producerThread.join(); // Block main until generation loop completely stops
        }

        std::cout << "Goodbye!\n";

    }
    catch (const std::exception& e) {
        std::cerr << "Exception occurred within runtime: "
            << e.what() << "\n";
        return -1;
    }

    return 0;
}