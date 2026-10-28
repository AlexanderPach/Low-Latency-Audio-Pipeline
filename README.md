# Lock-Free Audio Pipeline

A real-time audio streaming pipeline in C++ that generates a sine wave and pushes it to your speakers through a lock-free SPSC (single-producer single-consumer) ring buffer I built from scratch, instead of relying on OS locks or a generic queue.

I got the idea after covering concurrency and synchronization in my Operating Systems course and wanted to see how far I could push this design in practice.

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

Requires a C++20 compiler. Build with Visual Studio on Windows or CMake on Linux.

### Visual Studio (MSVC)

1. Set the build configuration to **Release (x64)**.
2. Project Properties → set C++ Language Standard to **C++20**.
3. Ctrl+F5 to build and run. `winmm.lib` and `ole32.lib` are linked automatically for the Windows multimedia driver.

### macOS / Linux

Linux builds and headless tests have been verified with GCC. Audio-device playback
on Linux and macOS builds remain unverified. See the CMake commands below.

## Tests

The test suite uses CTest and the C++ standard library, with no downloads or extra
test framework. It covers queue empty/full behavior, FIFO ordering, wraparound,
move-only values, object lifetimes, and concurrent transfers with both a two-slot
queue and the audio queue's 65,536-slot capacity. Audio-engine tests check callback
consumption, stop/restart, and destruction during playback using miniaudio's silent
backend. They do not need speakers or an audio server, and do not verify audible
output or the generated sine wave.

With a C++20 compiler and CMake 3.21 or newer, run from the repository root:

```sh
cmake -S . -B out/build/tests -DCMAKE_BUILD_TYPE=Debug -DBUILD_TESTING=ON
cmake --build out/build/tests --parallel 2
ctest --test-dir out/build/tests --output-on-failure
```

For a multi-configuration generator such as Visual Studio, add `--config Debug`
to the build command and `-C Debug` to CTest. Checks remain enabled in Release
builds. Use `ctest --test-dir out/build/tests -R '^spsc\.' --output-on-failure`
to run only the queue tests, or configure with `-DBUILD_TESTING=OFF` to omit the
test executables. CI builds and tests both Debug and Release on Linux.

## Measuring latency

Queue speed, time spent waiting in the queue, and speaker latency are different
measurements. At 44.1 kHz, a full 65,536-sample queue represents about **1.49 seconds
of buffered audio**. The producer eagerly fills the queue, so fast synchronization
alone does not establish low playback latency.

Optional benchmarks compare the SPSC queue with an equally sized, preallocated
mutex-protected ring. They measure operation cost, concurrent throughput, active
handoff latency, and queue residence in a paced consumer simulation. See
[the benchmark protocol and results](benchmarks/README.md) for commands, raw data,
limitations, and the hardware loopback measurements needed for playback claims.
