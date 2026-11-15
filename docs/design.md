# Queue and firmware-state invariants

This note records the invariants the code relies on.  They are the things to
check first when changing `queue.c`, `device_sim.c` or `fw_slots.c`.

## 1. Ownership

| State | Owner |
|---|---|
| `sq_tail_resv`, `sq_tail` | producers (CAS / release store) |
| `cq_head`, `cq_phase` | exactly one reaper thread per queue pair |
| `sq_head` (device) | the queue's executor thread |
| `cq_tail`, `cq_phase` (device) | the queue's executor thread |
| `dev->q[i].active`, `in_service` | controller thread + executor, release/acquire |
| `CC`, `AQA`, `ASQ`, `ACQ`, `CSTS`, doorbells | shared through BAR0 atomics |

A queue pair is single-producer/single-reaper in the workload and tests.  The
submission path still uses a compare-exchange reservation so that several
producers per queue stay safe; the completion path is strictly single-reaper
because the phase bit is shared state.

## 2. Doorbells are free-running 16-bit indices

The value written to an SQ tail doorbell is `(uint16_t)(sq_tail_resv + 1)`, not
`value % depth`.  The controller derives the memory slot with `index % depth`.
Masking the doorbell would make a completely full ring look empty (head ==
tail), so the free-running form is what makes `depth` outstanding commands
unambiguous.  Queue depths are therefore powers of two (the controller rejects
anything else), which keeps `index % depth` continuous across the 16-bit wrap.

## 3. Memory ordering

Publishing an SQE:

```
write SQE bytes
release fence
release store to SQ tail doorbell
```

The fence is required because the SQE bridge and the MMIO doorbell are two
independent stores; without it the controller may see the tail advance before
the SQE is visible.  The device acquire-loads the doorbell before reading the
SQE.

Publishing a CQE:

```
write cdw0, sqhd, sqid, cid
release store of the status word (contains the phase tag)
```

Reaping:

```
acquire load of status
if phase != expected: stop
acquire fence
read/copy the rest of the CQE
```

The host never reads a doorbell back: doorbells are write-only on real NVMe
controllers.

## 4. Completion-ring wrap

* The phase tag starts at 1.
* A CQE belongs to the consumer when `status & 1 == cq_phase`.
* `cq_phase` flips exactly once, when the masked index wraps to 0.
* The host rings the CQ head doorbell once per batch, not once per completion.
* On controller reset the CQ memory is cleared by the device and the host
  re-arms `cq_head = 0`, `cq_phase = 1`; this prevents a stale phase match.

## 5. Flow control

`inflight = sq_tail_resv - completed` (both free-running counters) and
`inflight >= depth` means `-EAGAIN`.  `cq_head` is a masked ring index and must
not be used in this calculation.  A full submission ring must never overwrite
an in-flight SQE; the CAS reservation plus the `-EAGAIN` path guarantees that.

## 6. Controller reset

`CC.EN 1 -> 0` tears down every I/O queue and the admin queue, clears all
doorbells, clears CQ memory and drops `CSTS.RDY`.  Before the host clears
`CC.EN` it must drain in-flight I/O; the workload `pause()` parks each queue
after its last completion and does not touch the rings while the reset runs.
A pending firmware activation (`afi.next`) is applied on the next enable.

## 7. Firmware slots

* Slot 1 is read-only (FRMW bit 0) and cannot be replaced by CA=0/1/3.
* A staged image is only accepted if the download offsets are strictly
  sequential (`ofst == staging_len`); offset 0 starts a new download.
* Commit validates magic, version, length and the payload SHA-256 before
  writing a slot.  A bad image discards staging and leaves the active slot
  untouched.
* CA=3 is split into `fw_mgr_prepare()` (validate, hash, write the idle slot)
  and `fw_mgr_apply()` (flip the active pointer).  Only `apply` runs inside the
  quiesce window, so the controller pause does not include the image hash.
* Failed post-activation self test restores the previous active slot and
  increments `rollback_count`; the target slot keeps the rejected revision so
  the log page tells the truth.
* The log page 0x03 is generated from the slot table on every read, so `afi`
  and all seven revision strings always reflect current state.
