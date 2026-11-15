# NVMeSlotSwap experiment results

Generated: 2026-11-15T05:31:57Z  

Host: macOS-27.2.1-arm64-arm-64bit-Mach-O  

Binary: `build/nvmeslotswap`  

Repetitions per scenario: 3 (median reported)


The device is the in-repo emulated NVMe controller; the driver, queues, doorbells, PRPs and firmware commands are the real code paths. Latency figures come from `LatencyRecorder` in the harness, not from the device model's own service-time parameter. The measurement host was a shared workstation, so `max_gap` also contains scheduler noise; the 3-run median is reported for every metric.


## Scenario comparison

| Scenario | Queues | IOPS | Completed | Dropped | Failed | Integrity errors | p50 | p99 | p99.9 | Max completion gap | Controller downtime |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (no firmware activity) | 4 | 482,350 | 971,195 | 0 | 0 | 0 | 139.3 µs | 1.84 ms | 5.77 ms | 13.65 ms | 0.0 µs |
| CA=3 activate immediately (no reset) | 4 | 447,812 | 905,374 | 0 | 0 | 0 | 139.3 µs | 2.23 ms | 4.19 ms | 5.48 ms | 0.0 µs |
| CA=2 activate on next reset | 4 | 406,679 | 822,586 | 0 | 0 | 0 | 139.3 µs | 2.88 ms | 7.34 ms | 10.06 ms | 2.81 ms |
| CA=3 with failed post-activation self test | 4 | 414,936 | 842,820 | 0 | 0 | 0 | 139.3 µs | 3.28 ms | 5.24 ms | 4.16 ms | 0.0 µs |

## Swap window versus steady state

The swap window is ±200 ms around the activation timestamp. The activation gap is the longest interval with no completions whose two ends fall in the 20 ms after the activation timestamp, which isolates the controller's own interruption from scheduler noise elsewhere in the run. Percentiles are median-of-3 across repetitions.

| Scenario | Steady p50 | Steady p99 | Steady p99.9 | Window p50 | Window p99 | Window p99.9 | Activation gap | Window samples |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CA=3 activate immediately (no reset) | 139.3 µs | 2.23 ms | 4.19 ms | 139.3 µs | 1.77 ms | 4.72 ms | 1.66 ms | 100,031 |
| CA=2 activate on next reset | 139.3 µs | 2.88 ms | 7.34 ms | 139.3 µs | 2.23 ms | 3.41 ms | 2.99 ms | 85,574 |
| CA=3 with failed post-activation self test | 139.3 µs | 3.28 ms | 5.24 ms | 139.3 µs | 3.41 ms | 6.03 ms | 3.25 ms | 57,615 |

## Queue scaling (CA=3)

| Queue pairs | IOPS | p99 | p99.9 | Max gap | Dropped | Integrity errors |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 238,435 | 278.5 µs | 1.02 ms | 12.82 ms | 0 | 0 |
| 2 | 431,974 | 393.2 µs | 2.49 ms | 4.14 ms | 0 | 0 |
| 4 | 491,044 | 1.90 ms | 4.06 ms | 6.03 ms | 0 | 0 |
| 8 | 236,694 | 9.96 ms | 24.12 ms | 7.40 ms | 0 | 0 |

## Rollback verification

- `failed_commit` runs: rollback observed in all repetitions; active slot before=[1] after=[1].
- Dropped commands: 0; integrity errors: 0; failed completions: 0.
- Successful CA=3 activation: active slot before=[1] after=[2], verified via log page 0x03 in all runs.

## Takeaway

- CA=3 moved the active slot with **0 controller resets** and **0 dropped commands** across the measured runs, at 447,812 IOPS.
- CA=2 needed a controller reset and queue re-creation costing 2.81 ms of controller downtime; its run delivered 406,679 IOPS.
- The failed commit rolled back to the prior slot with 0 dropped commands and 0 integrity errors, so the namespace stayed usable.

_Plots skipped: matplotlib is not installed in this Python.
_
