# Trial summaries

Each cell is the median [minimum, maximum] across trials. Latency cells summarize
per-trial quantiles; they are not quantiles of pooled samples. Maxima are observed,
not guaranteed bounds. Warmup rows are excluded.

## Queue operation costs and throughput

| Capacity | Queue | Trials | Serial pair (ns/pair) | Saturated transfer (million items/s) |
| ---: | --- | ---: | ---: | ---: |
| 256 | spsc | 5 | 2.274 [2.233, 2.341] | 59.100 [58.660, 323.490] |
| 256 | mutex | 5 | 13.212 [13.201, 14.611] | 9.285 [7.951, 11.266] |
| 65536 | spsc | 5 | 2.239 [2.223, 2.283] | 37.925 [36.756, 41.186] |
| 65536 | mutex | 5 | 13.429 [13.290, 14.013] | 14.106 [12.798, 16.175] |

## Instrumented sparse handoff

| Capacity | Queue | Samples/trial | p50 (µs) | p95 (µs) | p99 (µs) | Largest observed (µs) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: |
| 256 | spsc | 20000 | 0.406 [0.195, 0.426] | 0.536 [0.235, 0.556] | 0.576 [0.246, 0.606] | 196.132 |
| 256 | mutex | 20000 | 0.946 [0.210, 0.957] | 9.219 [0.401, 9.529] | 11.111 [0.480, 19.194] | 1499.468 |
| 65536 | spsc | 20000 | 0.415 [0.200, 0.431] | 0.526 [0.231, 0.551] | 0.565 [0.250, 0.576] | 178.969 |
| 65536 | mutex | 20000 | 0.911 [0.370, 1.141] | 1.186 [0.381, 9.394] | 3.140 [0.471, 13.460] | 215.999 |

## Paced simulation: generated-to-callback-completion age

| Capacity | Queue | Trials | p50 (ms) | p95 (ms) | p99 (ms) | Underruns / scheduled samples | Final occupancy (min–max) |
| ---: | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| 256 | spsc | 5 | 5.802 [5.801, 5.802] | 5.822 [5.817, 5.827] | 5.875 [5.859, 5.937] | 0 / 661760 | 17–39 |
| 256 | mutex | 5 | 5.802 [5.798, 5.805] | 5.827 [5.822, 5.869] | 5.875 [5.845, 7.913] | 256 / 661760 | 45–60 |
| 65536 | spsc | 5 | 1486.073 [1486.071, 1486.074] | 1486.093 [1486.089, 1486.103] | 1486.130 [1486.112, 1486.371] | 0 / 661760 | 65282–65327 |
| 65536 | mutex | 5 | 1486.072 [1486.070, 1486.076] | 1486.105 [1486.096, 1486.114] | 1486.153 [1486.122, 1486.801] | 0 / 661760 | 65296–65354 |

## Instrumented callback execution and scheduling

256-frame budget = 5.805 ms; callback cost includes validation and metadata copying.

| Capacity | Queue | Callback p99 (µs) | Wake lateness p99 (ms) | Callbacks over budget | Completions past next deadline |
| ---: | --- | ---: | ---: | ---: | ---: |
| 256 | spsc | 1.733 [1.412, 2.694] | 0.135 [0.108, 0.181] | 0/2585 | 0/2585 |
| 256 | mutex | 39.950 [28.202, 76.575] | 0.122 [0.110, 0.909] | 0/2585 | 0/2585 |
| 65536 | spsc | 2.153 [1.632, 2.423] | 0.123 [0.106, 0.219] | 0/2585 | 0/2585 |
| 65536 | mutex | 49.003 [26.269, 67.942] | 0.131 [0.106, 0.146] | 0/2585 | 0/2585 |
