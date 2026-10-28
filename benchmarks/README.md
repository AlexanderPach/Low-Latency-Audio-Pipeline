# Queue benchmarks and audio buffering

These experiments answer different questions. A fast queue is useful, but is not
itself evidence of low speaker latency or real-time deadline guarantees.

| Experiment | What it measures | What it does not measure |
| --- | --- | --- |
| Serial publish/consume pairs | Cost of one successful publish plus one consume on one thread, with correctness checks | Cross-thread latency |
| Saturated transfer | Successful float items transferred per second by one producer and one consumer | Audio playback latency; per-item latency is not the reciprocal of throughput |
| Sparse handoff | Instrumented publication-to-consumption time with one timestamped message in flight, both threads polling | Idle thread wakeup, backlog, or hardware output |
| Paced simulation | Generated-to-callback-completion age with an eager producer and a chosen 256-frame/44.1 kHz consumption schedule | The actual miniaudio callback, driver/device latency, sine generation cost, or acoustic latency |

## Baseline and controls

The baseline is a bounded, preallocated ring protected by `std::mutex`, with the
same capacity, FIFO behavior, and full/empty return values as `SPSCDisruptor`.
It takes one `lock_guard` per publish or consume. It can block acquiring the lock;
it does not wait for data/space using a condition variable. Both candidates yield
when retrying a failed queue operation. A dynamically allocating `std::queue`
would mix allocator and synchronization costs, so it is not the primary baseline.
This comparison is against one reasonable mutex implementation, not every queue.
The implementations also differ in cursor bookkeeping and memory layout; a
measured difference cannot be attributed exclusively to the mutex instruction.

Both queue capacities (256 and 65,536) are tested. Benchmark storage is allocated
before measurement, successful values/counts/order are checked, and measured
sections do not log. Warmups precede queue microbenchmarks. Candidate order
alternates between trials. Thread creation is outside the microbenchmark timed
interval. Raw CSV retains every trial and labels warmup rows separately. Results
use nearest-rank percentiles and show variation between runs; observed maxima are
not worst-case bounds. CI runs correctness smoke checks, not speed thresholds.

The sparse handoff uses a larger timestamp/sequence payload than the application's
four-byte float. Timestamp overhead is measured and logged, not subtracted from
the latency quantiles. An acknowledgement prevents more than one message being
in flight; it occurs after the measured interval but its cache traffic and polling
still affect the experiment. This is active handoff under optimistic polling
conditions, not time from an idle producer or consumer becoming runnable.

The paced simulation keeps float-sized queue entries by encoding sequence IDs
rather than sine samples. Generation timestamps use a separate, preallocated,
write-once array published through the queue's acquire/release synchronization.
The timestamp is taken before the producer first tries to enqueue a value, so
age includes a possible full-queue wait. Callback age is measured at the end of
the simulated batch, before device playback would happen. Clock/metadata stores,
value validation, and metadata copies perturb the workload. Callback execution
time therefore describes this instrumented simulation, not `maAudioCallback`.

The consumer sleeps to absolute block deadlines without silently discarding late
blocks. Wakeup lateness, callback execution time, completion after the next block
deadline, and silence-producing empty reads are separate metrics. A 256-frame
block at 44.1 kHz gives a 5.805 ms budget. That block size is an experiment choice:
`AudioEngine` does not request a particular device period. The default two-second
warmup exceeds the large queue's 1.486-second capacity time before ages are
recorded. Startup behavior is excluded, not established by steady-state results.
CSV warmup/measurement durations are the scheduled block intervals. Late wakeups
and catch-up execution can change elapsed wall time; the runner records process
start/end times separately.

## Reproduce

Use an optimized build. From the repository root, with a C++20 compiler, CMake
3.21+, and Python 3:

```sh
cmake -S . -B out/build/benchmarks -DCMAKE_BUILD_TYPE=Release \
  -DLLAP_BUILD_BENCHMARKS=ON -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
cmake --build out/build/benchmarks --parallel 2
ctest --test-dir out/build/benchmarks --output-on-failure
python3 benchmarks/run_benchmarks.py --build-dir out/build/benchmarks \
  --output-dir out/benchmarks/local-run --trials 5 --simulation-trials 5
python3 benchmarks/summarize.py out/benchmarks/local-run
```

Optional, with Matplotlib installed, generate a standalone SVG and PNG figure:

```sh
python3 benchmarks/plot_results.py out/benchmarks/local-run
```

Choose a new output directory each time; the runner refuses to overwrite an old
run. It runs workloads sequentially and preserves `queue.csv`, `simulation.csv`,
stderr logs, source/binary hashes, compiler commands, machine details, affinity,
and available CPU quota information. It does not dump environment variables or
credentials. Multi-configuration generators need `--config Release` when building
and `-C Release` for CTest, then a `--build-dir` pointing to the `Release` binary
directory for the Python runner. Compiler-command metadata may be unavailable
with those generators; record compiler version/flags separately.

Both executables accept `--help`. `queue_benchmark --self-test` checks baseline
correctness; `--quick` runs short smoke workloads and must not be used as evidence
of performance. Stop builds/tests and other local workloads before collecting
numbers. Do not run two benchmark instances at the same time. To compare CPU
pinning, background load, callback sizes, or retry policies, treat each as a new
experiment and document the change for both candidates.

## What the application currently implies

`AudioEngine::kBufferSize` is 65,536 mono samples and the producer uses 44,100
samples per second. A full queue therefore holds:

```text
65,536 / 44,100 = 1.486077 seconds
256 / 44,100 = 0.005805 seconds
```

The producer generates as fast as it can until the queue is full. If it stays
ahead of the consumer, that design trades a large amount of lookahead for underrun
protection. Capacity is a derived buffering limit; the paced experiment measures
residence under its own assumptions. A continuous, predetermined sine wave has
no external input event from which to measure input-to-output responsiveness.
Changing capacity alone can reduce buffering but also increase underrun risk.
Do not silently shrink it and describe the original program as low latency.

For a physical playback claim, run on the intended machine/audio device and
record the backend, actual sample rate, device periods/count, power settings,
CPU load, and underruns. Introduce an identifiable input/control marker and
measure when it reaches the output using an electrical audio loopback or a
separate acquisition device with a common timing reference. Record repeated
marker delays and p50/p95/p99, including startup and load conditions separately.
An electrical output-to-input loopback measures round-trip delay, including
capture; do not label it output-only delay or subtract an unmeasured capture
delay. A silent backend cannot validate any of these hardware claims.

## Recorded results

See [the cloud measurement report](results/cloud-2026-10-28/REPORT.md) for the
recorded environment, every trial, and the conclusions these data support.
