# NVMeSlotSwap

Dual-image (A/B slot) NVMe firmware activation that does not interrupt host
I/O, with a measurement harness that reports exactly what the host observes
while the active firmware slot changes.

The project is a proof of concept for on-the-fly SSD firmware upgrade in a
datacenter: no controller reset, no queue teardown, no dropped commands.  It
implements the real NVMe mechanics behind that claim — 64-byte submission queue
entries, 16-byte completion queue entries with phase-tag detection, doorbell
registers in BAR0, admin and I/O queue pairs, Identify Controller/Namespace,
Firmware Image Download (opcode 0x11), Firmware Commit (opcode 0x10) with all
four commit actions, and the Firmware Slot Information log page 0x03 — and it
verifies every firmware image with a from-scratch SHA-256 before a slot is
touched.

## Architecture

```
+------------------------------ host process -------------------------------+
|  workload.cpp   N worker threads, one queue pair each, QD outstanding,    |
|                 mixed read/write, per-LBA integrity patterns, latency     |
|                 histogram, longest-zero-completion-gap tracker            |
|        |                                                                  |
|  driver.cpp     NvmeDriver + FirmwareUpdater (C++17 facade): CC.EN        |
|                 sequence, Identify, Create/Delete SQ/CQ, PRP submission,  |
|                 FW Image Download / Commit, log page 0x03                 |
|        |                                                                  |
|  queue.c        lock-free SQ/CQ rings: CAS slot reservation, phase-tag    |
|                 completion detection, release/acquire fences, -EAGAIN     |
|                 backpressure, per-queue stall/in-flight counters          |
|        |                                                                  |
|  backend.hpp    Backend seam: bar0(), dma_alloc(), dma_free(), stride     |
|        |                                                                  |
|        +---- SimBackend (default, every host)                             |
|        |                                                                  |
|        +---- VfioBackend (Linux, compile-verified only)                   |
+--------|------------------------------------------------------------------+
         |
         |  BAR0 MMIO + shared "DMA" arena        (VFIO: BAR0 mmap + IOMMU)
         v
+----------------------------------------+     +---------------------------+
| device_sim.c  controller thread        |     | real or QEMU-emulated     |
|   CC/CSTS/AQA/ASQ/ACQ state machine    |     | NVMe controller           |
|   admin executor: queues, Identify,    |     | (VFIO path)               |
|     Get Log Page, FW 0x10/0x11         |     +---------------------------+
|   one I/O executor per queue pair,     |
|     PRP1 / PRP2 / PRP-list walking     |
| fw_slots.c    7 slots, slot 1 read-only|
|   4 KiB chunk staging, SHA-256 verify, |
|   quiesce + activation + rollback      |
| namespace store: ns_lba_count x 4 KiB  |
+----------------------------------------+
```

**Memory-ordering contract.**  The SQE bytes and the SQ tail doorbell are two
independent stores, so `nvme_qp_submit()` writes the 64-byte SQE, executes a
release fence, and only then release-stores the doorbell.  The controller
acquire-loads the doorbell before reading the SQE.  On the completion side the
phase tag in the status word is the only field read before validity is known:
`nvme_qp_reap()` acquire-loads the status, compares the phase, then executes an
acquire fence before copying the rest of the CQE.  The device release-stores the
status word after all other CQE fields.  Doorbells are never read back by the
host — they are write-only registers on real controllers — and the values are
free-running 16-bit indices (the memory slot is `index % depth`), so a full ring
is not mistaken for an empty one.  Queue depths are powers of two to keep that
modulo arithmetic continuous across the 16-bit wrap.  All atomics use the
compiler's `__atomic` builtins, which means the same code is race-free under
ThreadSanitizer while still modelling MMIO ordering.  The full invariant list is
in `docs/design.md`.

## Firmware upgrade state machine

A staged image is `struct nvme_fw_image_hdr` (magic `NVFW`, version, 8-byte
revision, payload length, SHA-256 of the payload) followed by the payload.  The
host downloads it in 4 KiB chunks with FW Image Download (0x11), programming
`NUMD` and `OFST` in dwords; the device accepts only strictly sequential
offsets, rejects overlaps, and caps the image at 4 MiB.  Commit (0x10)
validates magic, version, length and SHA-256 **before** any slot is written.  A
bad image returns `FW_IMAGE_INVALID`, discards staging, and leaves the active
slot untouched.

| CA | Action | Device behaviour | Completion status |
|---:|---|---|---|
| 0 | Replace slot, no activation | Validate image, write the idle slot, active and next unchanged | `SUCCESS` (0x00) |
| 1 | Replace slot, activate on next reset | Validate and write the slot, `afi.next = slot` | `SUCCESS` (0x00) |
| 2 | Activate an existing slot on next reset | `afi.next = slot`; activation deferred | `FW_ACTIVATION_REQUIRES_RESET` (0x0B) |
| 3 | Replace slot and activate immediately | Validate/write the idle slot first, quiesce I/O, flip `afi.active`, self-test | `SUCCESS` (0x00), or rollback + `FW_IMAGE_INVALID` (0x07) |

Additional rules: replacing slot 1 returns `FW_ACTIVATION_PROHIBITED` (0x13)
because FRMW bit 0 advertises it read-only; a slot number above 7, or CA=2 on an
empty slot, returns `FW_SLOT_INVALID` (0x06).

The CA=3 flow is:

```
build image (header + payload + SHA-256)
  -> stage in 4 KiB chunks (0x11 / NUMD / OFST)          [I/O unaffected]
  -> commit (0x10, CA=3)
       validate magic, length, SHA-256, write idle slot  [I/O unaffected]
       quiesce: stop fetching new SQEs for 1.5 ms
       flip active slot, update log page 0x03
  -> post-activation self test
       pass  : success, no reset
       fail  : pulse CSTS.CFS, restore previous active slot, report rollback
  -> host re-reads log page 0x03 and verifies the active revision
```

Commands that were already queued when the quiesce starts are not dropped or
failed: executors finish the command in service and park until the quiesce ends.
That is the difference from CA=2, which needs a full `CC.EN 1->0` controller
reset and queue re-creation.

## How to run

Requirements: CMake >= 3.20, a C11/C++17 compiler, POSIX threads, Python 3.
GoogleTest is fetched by CMake.  The default build is the in-process emulated
controller, so it runs on macOS and Linux with no hardware.

```bash
cmake -S . -B build
cmake --build build -j
ctest --test-dir build --output-on-failure      # 23 tests
```

### CLI

```bash
./build/nvmeslotswap info
# model: NVMeSlotSwap Emulated Controller
# serial: NVMESS0000000000001
# firmware: FW1.0.0
# namespaces: 1

./build/nvmeslotswap ident
# model:      NVMeSlotSwap Emulated Controller
# serial:     NVMESS0000000000001
# firmware:   FW1.0.0
# vendor:     0x1b36 subsystem: 0x1b36
# namespace:  1, 16384 LBAs x 4096 bytes (64 MiB)
# lba format: lbads=12 (4096-byte LBAs)

./build/nvmeslotswap io --queues 4 --depth 64 --seconds 3 --rw 70
# io: queues=4 depth=64 seconds=3.00 rw=70
# iops: 439825
# submitted: 1319624 completed: 1319624 failed: 0 eagain_stalls: 0
```

Firmware commands share a small persisted image of the emulated controller's
slot table under `.nvmess/fw.state`, so `stage`, `commit` and `list` observe the
same part across process invocations (the device itself is in-process; the
namespace is not persisted).

```bash
./build/nvmeslotswap fw list
# active slot: 1   next activation slot: 1
#   slot 1: FW1.0.0  [active]  [read-only]
#   slot 2: (empty)
#   ...
# emulated device state: .nvmess/fw.state (not found; fresh controller)

./build/nvmeslotswap fw stage --slot 2 --rev FW2.0.1 --size 1048576
# staged 1048628 bytes in 257 x 4096-byte chunks for slot 2 (rev FW2.0.1) in 11989 us
# active slot: 1   next activation slot: 1

./build/nvmeslotswap fw commit --slot 2 --action 3
# commit: CA=3 replace slot and activate immediately
# status: 0x00   active slot: 2 (FW2.0.1)   next slot: 2   latency: 5569 us
# controller resets during commit: 0
# activation verified via log page 0x03: yes
# namespace check: OK (2 LBAs written before commit read back intact)

./build/nvmeslotswap fw stage --slot 3 --rev FW3.0.0 --size 1048576
./build/nvmeslotswap fw commit --slot 3 --action 3 --fail-commit
# commit: CA=3 replace slot and activate immediately
# status: 0x07   active slot: 2 (FW2.0.1)   next slot: 2   latency: 7815 us
# controller resets during commit: 0
# ROLLBACK: post-activation self test failed; restored slot 2 (FW2.0.1)
# namespace check: OK (2 LBAs written before commit read back intact)
```

CA=2 on an already-installed slot, with the reset performed by the CLI:

```bash
./build/nvmeslotswap fw commit --slot 2 --action 0        # install only
./build/nvmeslotswap fw commit --slot 2 --action 2 --reset
# status: 0x0b ... activation pending ... after reset: active slot 2 (FW2.0.1)
```

`swap-test` runs continuous I/O, stages and commits under load, and emits one
JSON object (or writes it with `--json PATH`).  This is a real run with 4 queues
and depth 32:

```bash
./build/nvmeslotswap swap-test --queues 4 --depth 32 --warmup-ms 1000 --post-ms 1000 --action 3
# swap-test: action=3 iops=495496 completed=1000262 dropped=0 failed=0 integrity_errors=0 max_gap=5673.8us p99=1966.08us downtime=0us
```

```json
{
  "config": {"queues": 4, "depth": 32, "warmup_ms": 1000, "post_ms": 1000,
             "rw": 70, "seed": 12345, "slot": 2, "rev": "FW2.0.1",
             "size": 262144, "action": 3, "fail_commit": false, "stall_us": 0},
  "activation_action": 3,
  "activation_latency_us": 3634.875,
  "controller_downtime_us": 0.0,
  "submitted": 1000262,
  "completed": 1000262,
  "dropped": 0,
  "failed": 0,
  "failed_by_status": {},
  "integrity_errors": 0,
  "eagain_stalls": 0,
  "max_gap_us": 5673.834,
  "iops": 495496.293,
  "latency_us": {"count": 1000262, "mean": 244.890, "p50": 139.264,
                 "p90": 442.368, "p99": 1966.080, "p999": 4194.304,
                 "max": 16243.667},
  "swap_window_latency_us": {"count": 115472, "mean": 209.142, "p50": 139.264,
                 "p90": 360.448, "p99": 1376.256, "p999": 3407.872,
                 "max": 5782.583},
  "swap_window_max_gap_us": 3424.958,
  "activation_gap_us": 3424.958,
  "active_slot_before": 1,
  "active_slot_after": 2,
  "rollback_occurred": false,
  "activation_verified": true,
  "stage_us": 2066,
  "prefill_ms": 44.372,
  "duration_s": 2.019,
  "namespace_lbas": 16384,
  "host": "Darwin 27.2.0 arm64 (8 cpus)"
}
```

### Experiments

```bash
python3 tools/run_experiments.py            # 3 repetitions per scenario
python3 tools/run_experiments.py --quick    # CI-sized pass
```

The harness builds if needed, runs baseline, CA=3, CA=2, failed-commit and 1/2/4/8
queue scaling, writes `results/raw/*.json`, `results/summary.json` and
`results/report.md`, and exits nonzero if any no-reset scenario reports a
dropped command or an integrity error.  If matplotlib is present it also writes
`results/latency_cdf.png` and `results/queue_scaling.png`.

### Sanitizers and the VFIO build

```bash
cmake -S . -B build-asan -DNVMESS_SANITIZE=ON && cmake --build build-asan -j
ctest --test-dir build-asan --output-on-failure

cmake -S . -B build-tsan -DNVMESS_TSAN=ON
cmake --build build-tsan -j --target test_concurrency
./build-tsan/tests/test_concurrency

cmake -S . -B build-vfio -DNVMESS_BACKEND_VFIO=ON   # Linux only
```

`tools/ci_run.sh` performs all of the above plus a short experiment run.
`docs/qemu.md` has the QEMU invocation and the `vfio-pci` bind steps for the
Linux transport path.

## Results

Measured on an Apple M3 (8 cores), macOS 27.2.1 (Darwin 27.2.0), Apple clang
21.0.0, CMake 4.3.3.  The device is the in-repo emulated NVMe controller; the
driver, rings, doorbells, PRPs and firmware commands are the real code paths.
The host was a shared workstation, so completion gaps also contain scheduler
noise.  Every figure below is the median of 3 runs and is copied from
`results/report.md`; the raw runs are in `results/raw/`.

### Scenario comparison

| Scenario | Queues | IOPS | Completed | Dropped | Failed | Integrity errors | p50 | p99 | p99.9 | Max completion gap | Controller downtime |
|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| baseline (no firmware activity) | 4 | 453,311 | 917,601 | 0 | 0 | 0 | 139.3 µs | 2.23 ms | 4.72 ms | 4.85 ms | 0.0 µs |
| CA=3 activate immediately (no reset) | 4 | 438,897 | 897,612 | 0 | 0 | 0 | 139.3 µs | 2.49 ms | 5.77 ms | 5.94 ms | 0.0 µs |
| CA=2 activate on next reset | 4 | 437,523 | 887,323 | 0 | 0 | 0 | 139.3 µs | 2.23 ms | 6.03 ms | 8.14 ms | 3.31 ms |
| CA=3 with failed post-activation self test | 4 | 408,892 | 824,290 | 0 | 0 | 0 | 139.3 µs | 3.28 ms | 4.72 ms | 5.51 ms | 0.0 µs |

### Swap window versus steady state

The swap window is ±200 ms around the activation timestamp.  The activation gap
is the longest interval with no completions whose two ends fall in the 20 ms
after the activation timestamp; it isolates the controller's own interruption
from scheduler noise elsewhere in the run.

| Scenario | Steady p50 | Steady p99 | Steady p99.9 | Window p50 | Window p99 | Window p99.9 | Activation gap | Window samples |
|---|---:|---:|---:|---:|---:|---:|---:|---:|
| CA=3 activate immediately (no reset) | 139.3 µs | 2.49 ms | 5.77 ms | 139.3 µs | 2.62 ms | 3.67 ms | 2.22 ms | 92,947 |
| CA=2 activate on next reset | 139.3 µs | 2.23 ms | 6.03 ms | 139.3 µs | 1.44 ms | 4.46 ms | 3.85 ms | 105,653 |
| CA=3 with failed post-activation self test | 139.3 µs | 3.28 ms | 4.72 ms | 139.3 µs | 3.67 ms | 6.55 ms | 1.46 ms | 71,336 |

### Queue scaling (CA=3)

| Queue pairs | IOPS | p99 | p99.9 | Max gap | Dropped | Integrity errors |
|---:|---:|---:|---:|---:|---:|---:|
| 1 | 192,425 | 819.2 µs | 9.96 ms | 11.16 ms | 0 | 0 |
| 2 | 388,708 | 589.8 µs | 2.49 ms | 5.94 ms | 0 | 0 |
| 4 | 394,888 | 3.41 ms | 8.91 ms | 8.20 ms | 0 | 0 |
| 8 | 262,810 | 9.44 ms | 18.87 ms | 10.05 ms | 0 | 0 |

Scaling flattens at 8 queue pairs because 8 workload threads and 8 device
executor threads share the 8 cores of the measurement host (plus the controller
thread); this is CPU saturation of the emulation, not a controller limit.

### Rollback verification

`failed_commit` runs (`CA=3` with the post-activation self test forced to fail):
rollback observed in all 3 repetitions, active slot before `[1]` and after `[1]`,
0 dropped commands, 0 integrity errors, 0 failed completions.  Successful CA=3
activation moved the slot `[1] -> [2]` and was verified through log page 0x03 in
all runs.

### Verification

- `ctest --test-dir build --output-on-failure`: **23 tests, all passing**.
- The same 23 tests pass in a Debug + AddressSanitizer + UndefinedBehaviorSanitizer
  build, with no findings.
- `Concurrency.FourQueuesTwentyThousandCommandsNoLoss` passes under
  ThreadSanitizer with no reports.

**Takeaway:** CA=3 changed the active slot with 0 controller resets, 0 dropped
commands and 0 integrity errors at 438,897 IOPS, interrupting completions for
2.22 ms; CA=2 required a 3.31 ms controller reset and queue re-creation, at
437,523 IOPS.

## Limitations and next steps

- The measured device is the in-repo emulated controller (a host-memory BAR0 and
  DMA arena), not silicon.  The VFIO backend is compile-verified only; no result
  here comes from it.
- One namespace, 4 KiB LBAs, a single controller.  No namespace management,
  sanitize, or DSM.
- Completions are polled.  There is no MSI-X/interrupt path, so interrupt
  coalescing and ISR latency are not modelled.
- The driver facade submits at most two 4 KiB pages per command (PRP1 + PRP2);
  the device and its tests do support PRP lists, and the workload's 8 KiB
  commands exercise PRP2 on every I/O.
- The device model's service time is a modelled delay, not measured media
  latency; the harness measures the completion path above it.
- Queue depths must be powers of two in this implementation because doorbells
  are free-running 16-bit indices.
- The CLI's `.nvmess/fw.state` is a convenience for the emulated, in-process
  part; it is not a persistent device.  The namespace is not part of it.
- Next: bind the queue layer to `VfioBackend` (the transport is already there),
  add MSI-X, model multiple namespaces, and validate against a real SSD that
  implements FW Image Download/Commit.
