# NVMeSlotSwap experiment results

Generated: 2026-11-15T05:38:33Z  

Host: macOS-27.2.1-arm64-arm-64bit-Mach-O  

Binary: `build/nvmeslotswap`  

Repetitions per scenario: 3 (median reported)


The device is the in-repo emulated NVMe controller; the driver, queues, doorbells, PRPs and firmware commands are the real code paths. Latency figures come from `LatencyRecorder` in the harness, not from the device model's own service-time parameter. The measurement host was a shared workstation, so `max_gap` also contains scheduler noise; the 3-run median is reported for every metric.


## Scenario comparison

| Scenario | Queues | IOPS | Completed | Dropped | Failed | Integrity errors | p50 | p99 | p99.9 | Max completion gap | Controller downtime |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (no firmware activity) | 4 | 453,311 | 917,601 | 0 | 0 | 0 | 139.3 µs | 2.23 ms | 4.72 ms | 4.85 ms | 0.0 µs |
| CA=3 activate immediately (no reset) | 4 | 438,897 | 897,612 | 0 | 0 | 0 | 139.3 µs | 2.49 ms | 5.77 ms | 5.94 ms | 0.0 µs |
| CA=2 activate on next reset | 4 | 437,523 | 887,323 | 0 | 0 | 0 | 139.3 µs | 2.23 ms | 6.03 ms | 8.14 ms | 3.31 ms |
| CA=3 with failed post-activation self test | 4 | 408,892 | 824,290 | 0 | 0 | 0 | 139.3 µs | 3.28 ms | 4.72 ms | 5.51 ms | 0.0 µs |

## Swap window versus steady state

The swap window is ±200 ms around the activation timestamp. The activation gap is the longest interval with no completions whose two ends fall in the 20 ms after the activation timestamp, which isolates the controller's own interruption from scheduler noise elsewhere in the run. Percentiles are median-of-3 across repetitions.

| Scenario | Steady p50 | Steady p99 | Steady p99.9 | Window p50 | Window p99 | Window p99.9 | Activation gap | Window samples |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CA=3 activate immediately (no reset) | 139.3 µs | 2.49 ms | 5.77 ms | 139.3 µs | 2.62 ms | 3.67 ms | 2.22 ms | 92,947 |
| CA=2 activate on next reset | 139.3 µs | 2.23 ms | 6.03 ms | 139.3 µs | 1.44 ms | 4.46 ms | 3.85 ms | 105,653 |
| CA=3 with failed post-activation self test | 139.3 µs | 3.28 ms | 4.72 ms | 139.3 µs | 3.67 ms | 6.55 ms | 1.46 ms | 71,336 |

## Queue scaling (CA=3)

| Queue pairs | IOPS | p99 | p99.9 | Max gap | Dropped | Integrity errors |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 192,425 | 819.2 µs | 9.96 ms | 11.16 ms | 0 | 0 |
| 2 | 388,708 | 589.8 µs | 2.49 ms | 5.94 ms | 0 | 0 |
| 4 | 394,888 | 3.41 ms | 8.91 ms | 8.20 ms | 0 | 0 |
| 8 | 262,810 | 9.44 ms | 18.87 ms | 10.05 ms | 0 | 0 |

## Rollback verification

- `failed_commit` runs: rollback observed in all repetitions; active slot before=[1] after=[1].
- Dropped commands: 0; integrity errors: 0; failed completions: 0.
- Successful CA=3 activation: active slot before=[1] after=[2], verified via log page 0x03 in all runs.

## Takeaway

- CA=3 moved the active slot with **0 controller resets** and **0 dropped commands** across the measured runs, at 438,897 IOPS.
- CA=2 needed a controller reset and queue re-creation costing 3.31 ms of controller downtime; its run delivered 437,523 IOPS.
- The failed commit rolled back to the prior slot with 0 dropped commands and 0 integrity errors, so the namespace stayed usable.

_Plots skipped: matplotlib is not installed in this Python.
_
