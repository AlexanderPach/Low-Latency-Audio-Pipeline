# Lock-Free Audio Pipeline

This project is a high-performance C++ real-time audio streaming pipeline. It uses a custom-built, lock-free Single-Producer Single-Consumer (SPSC) ring buffer to send mathematically generated sound directly to the audio hardware without using traditional OS locks.
Source for SPSCDisruptor: https://towardsdev.com/disruptor-style-queues-in-cpp-for-low-latency-software-835721d644dc

---

## How It Works

The application splits the work into two independent threads that talk to each other through the ring buffer:

1. **The Producer Thread:** Runs in the background, calculates a continuous math formula for a $440\text{ Hz}$ sine wave (a standard musical Note A), and pushes these raw data points into the buffer.
2. **The Consumer Thread:** Managed automatically by the **Miniaudio** library. It constantly pulls data points out of the buffer and sends them straight to your speakers.

---

## Core Technical Features

Instead of using a generic queue, the `SPSCDisruptor` ring buffer was built from scratch with several low-level optimizations:

* **Lock-Free with Atomics:** Uses `std::atomic` variables with precise memory orders (`acquire`/`release`) to sync data between threads safely without ever pausing the CPU with a mutex.
* **No False Sharing (`alignas`):** The internal tracking pointers are aligned to the CPU's hardware cache lines. This prevents the two threads from accidentally fighting over the same CPU cache memory.
* **Bitwise Array Wrapping:** The buffer size is strictly restricted to a power of 2 at compile-time. This allows the code to replace slow division math (`%`) with an ultra-fast bitwise AND (`&`) operation to wrap indices instantly.
* **Zero Runtime Memory Allocation:** The buffer uses a fixed-size `std::array` allocated once at startup. Data is constructed directly inside this memory using placement `new`, meaning no slow heap allocations occur while audio is playing.

---

## Building the Project

### Requirements
* A compiler that supports C++20.
* Works on Windows (MSVC), macOS, or Linux.

### Visual Studio (MSVC) Setup
1. Switch the build configuration at the top from **Debug** to **Release** (x64).
2. Right-click the project -> Properties -> Set the C++ Language Standard to **C++20**.
3. Press **Ctrl + F5** to run. The code automatically instructs Visual Studio to link the necessary Windows multimedia driver libraries (`winmm.lib` and `ole32.lib`).
3a. Given that information, I can only confirm for it work on Windows. If you want to run it on Linux or macOS, you may need to adjust the audio code.