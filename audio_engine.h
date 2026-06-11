#pragma once

#define MINIAUDIO_IMPLEMENTATION
#include "miniaudio.h"
#include "disruptor.h"
#include "SPSCDisruptor.cpp"

#include <iostream>
#include <stdexcept>

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "ole32.lib")

class AudioEngine {
public:
	// SPSCDirsuptor with a buffer size of 65536 samples (1 << 16), which is a common power-of-2 size for audio buffers
    static constexpr std::size_t kBufferSize = 1 << 16; 
    using AudioQueue = SPSCDisruptor<float, kBufferSize>;

    AudioEngine() {
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);

        deviceConfig.playback.format = ma_format_f32; 
        deviceConfig.playback.channels = 1; 
        deviceConfig.sampleRate = 44100;        
        deviceConfig.dataCallback = maAudioCallback;
        deviceConfig.pUserData = &queue_;

        if (ma_device_init(nullptr, &deviceConfig, &device_) != MA_SUCCESS) {
            throw std::runtime_error("Failed to initialize audio hardware device.");
        }
    }

    ~AudioEngine() {
        ma_device_uninit(&device_);
    }

    // Delete copy and move semantics so the underlying ma_device can't be duplicated or orphaned
    AudioEngine(const AudioEngine&) = delete;
    AudioEngine& operator=(const AudioEngine&) = delete;

    void start() {
        if (ma_device_start(&device_) != MA_SUCCESS) {
            throw std::runtime_error("Failed to start audio playback pipeline.");
        }
    }

    void stop() {
        if (ma_device_stop(&device_) != MA_SUCCESS) {
            throw std::runtime_error("Failed to stop audio playback pipeline.");
        }
    }

    AudioQueue& get_queue() {
        return queue_;
    }

private:
    static void maAudioCallback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
        float* outputBuffer = static_cast<float*>(pOutput);
        AudioQueue* engineQueue = static_cast<AudioQueue*>(pDevice->pUserData);

        if (!engineQueue || !outputBuffer) return;

        // Populate the hardware sound buffer ring-slot by ring-slot
        for (ma_uint32 i = 0; i < frameCount; ++i) {
            auto sample = engineQueue->try_consume();

            if (sample.has_value()) {
                outputBuffer[i] = sample.value();
            }
            else {
                // Genuinely empty: Output dead silence to avoid nasty speaker clicking/clipping
                outputBuffer[i] = 0.0f;
            }
        }
    }

    ma_device device_;
    AudioQueue queue_;
};