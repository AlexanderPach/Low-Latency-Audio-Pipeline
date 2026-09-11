# Lock-Free Audio Pipeline

A real-time audio streaming pipeline in C++ that generates a sine wave and pushes it to your speakers through a lock-free SPSC (single-producer single-consumer) ring buffer I built from scratch, instead of relying on OS locks or a generic queue.

I got the idea after covering concurrency and synchronization in my Operating Systems course and wanted to see how far I could push a "no locks, no allocations" design in practice.

Ring buffer design based on [this article on Disruptor-style queues in C++](https://towardsdev.com/disruptor-style-queues-in-cpp-for-low-latency-software-835721d644dc).

## How it works

Two threads talk to each other through the ring buffer:

- **Producer thread** — runs in the background, computes a 440 Hz sine wave (musical note A) sample by sample, and writes it into the buffer.
- **Consumer thread** — owned by [miniaudio](https://miniaud.io/), which pulls samples out of the buffer and sends them to the audio device.

## Why a custom ring buffer

A `std::queue` with a mutex would work, but it isn't safe for real-time audio — a lock can stall the audio thread and cause audible glitches. `SPSCDisruptor` avoids that:

- **Lock-free** — synchronization is done with `std::atomic` and acquire/release memory ordering instead of a mutex, so the audio thread never blocks.
- **No false sharing** — the read/write indices are `alignas`-padded to a cache line so the two threads aren't invalidating each other's CPU cache on every access.
- **Power-of-2 sizing** — buffer capacity is fixed to a power of 2 at compile time, so index wraparound is a bitwise `&` instead of a `%`.
- **Zero runtime allocation** — backed by a fixed `std::array`, with samples constructed in place via placement `new`. No heap activity once audio starts.

## Building

Requires a C++20 compiler. Tested on Windows; should be portable to macOS/Linux with some changes to the audio backend (see below).

### Visual Studio (MSVC)

1. Set the build configuration to **Release (x64)**.
2. Project Properties → set C++ Language Standard to **C++20**.
3. Ctrl+F5 to build and run. `winmm.lib` and `ole32.lib` are linked automatically for the Windows multimedia driver.

### macOS / Linux

Not yet tested. Miniaudio itself supports both platforms, but I haven't verified the build here — if you try it, I'd be curious to hear what breaks.
