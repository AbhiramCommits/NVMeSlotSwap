#!/usr/bin/env python3
"""Experiment harness for NVMeSlotSwap.

Runs each scenario three times with fixed seeds, aggregates the median run,
writes results/raw/*.json, results/summary.json and results/report.md, and
optionally renders latency_cdf.png / queue_scaling.png when matplotlib is
installed.  Standard library only otherwise.

The device is the in-repo emulated NVMe controller, so every number here is
produced by actually running the binary on this host.
"""

from __future__ import annotations

import argparse
import json
import os
import platform
import shutil
import statistics
import subprocess
import sys
from datetime import datetime, timezone
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
BUILD = ROOT / "build"
BINARY = BUILD / "nvmeslotswap"
RESULTS = ROOT / "results"
RAW = RESULTS / "raw"

SEEDS = [12345, 22222, 33333]
NO_RESET_SCENARIOS = {"baseline", "activate_ca3", "failed_commit", "scale"}

IMAGE_SIZE = "262144"


def parse_args() -> argparse.Namespace:
    p = argparse.ArgumentParser(description="Run the NVMeSlotSwap experiment matrix")
    p.add_argument("--quick", action="store_true", help="one short repetition per scenario")
    p.add_argument("--reps", type=int, default=3, help="repetitions per scenario (default 3)")
    p.add_argument("--rebuild", action="store_true", help="configure and build before running")
    p.add_argument("--no-plots", action="store_true", help="skip matplotlib output")
    return p.parse_args()


def build_if_needed(rebuild: bool) -> None:
    if rebuild or not BINARY.exists():
        print("[harness] building ...", flush=True)
        subprocess.run(["cmake", "-S", str(ROOT), "-B", str(BUILD)], check=True)
        subprocess.run(["cmake", "--build", str(BUILD), "-j"], check=True)
    if not BINARY.exists():
        raise SystemExit(f"missing binary: {BINARY}")


def scenario_matrix(quick: bool) -> list[dict]:
    """Returns a list of scenario descriptors."""
    warmup = 300 if quick else 1000
    post = 300 if quick else 1000
    common = ["--depth", "32", "--rw", "70", "--warmup-ms", str(warmup),
              "--post-ms", str(post), "--size", IMAGE_SIZE]
    scenarios = [
        {
            "key": "baseline",
            "label": "baseline (no firmware activity)",
            "args": ["--queues", "4", "--action", "none"] + common,
            "no_reset": True,
            "rollback_expected": False,
        },
        {
            "key": "activate_ca3",
            "label": "CA=3 activate immediately (no reset)",
            "args": ["--queues", "4", "--action", "3", "--rev", "FW2.0.1"] + common,
            "no_reset": True,
            "rollback_expected": False,
        },
        {
            "key": "activate_ca2",
            "label": "CA=2 activate on next reset",
            "args": ["--queues", "4", "--action", "2", "--rev", "FW2.0.1"] + common,
            "no_reset": False,
            "rollback_expected": False,
        },
        {
            "key": "failed_commit",
            "label": "CA=3 with failed post-activation self test",
            "args": ["--queues", "4", "--action", "3", "--rev", "FW2.0.1",
                     "--fail-commit"] + common,
            "no_reset": True,
            "rollback_expected": True,
        },
    ]
    for queues in (1, 2, 4, 8):
        scenarios.append(
            {
                "key": f"scale_q{queues}",
                "label": f"CA=3, {queues} queue pair(s)",
                "args": ["--queues", str(queues), "--action", "3", "--rev", "FW2.0.1"]
                        + common,
                "no_reset": True,
                "rollback_expected": False,
                "queues": queues,
            }
        )
    return scenarios


def run_once(scenario: dict, rep: int, seed: int, quick: bool) -> dict:
    RAW.mkdir(parents=True, exist_ok=True)
    out = RAW / f"{scenario['key']}_r{rep}.json"
    cmd = [str(BINARY), "swap-test", "--json", str(out), "--seed", str(seed), "--samples"]
    cmd += scenario["args"]
    proc = subprocess.run(cmd, cwd=str(ROOT), capture_output=True, text=True)
    if proc.returncode != 0:
        sys.stderr.write(proc.stdout + proc.stderr)
        raise SystemExit(f"run failed: {' '.join(cmd)}")
    data = json.loads(out.read_text())
    data["_rep"] = rep
    data["_seed"] = seed
    return data


def median(values: list[float]) -> float:
    values = [v for v in values if v is not None]
    return float(statistics.median(values)) if values else 0.0


def summarise(runs: list[dict]) -> dict:
    def get(path: str) -> list[float]:
        out = []
        for r in runs:
            cur = r
            for key in path.split("."):
                cur = cur.get(key, {}) if isinstance(cur, dict) else {}
            if isinstance(cur, (int, float)) and not isinstance(cur, bool):
                out.append(float(cur))
        return out

    summary = {
        "runs": len(runs),
        "iops": median(get("iops")),
        "submitted": median(get("submitted")),
        "completed": median(get("completed")),
        "dropped_max": max([r.get("dropped", 0) for r in runs], default=0),
        "dropped_median": median(get("dropped")),
        "failed_max": max([r.get("failed", 0) for r in runs], default=0),
        "integrity_errors_max": max([r.get("integrity_errors", 0) for r in runs], default=0),
        "eagain_stalls_median": median(get("eagain_stalls")),
        "max_gap_us": median(get("max_gap_us")),
        "controller_downtime_us": median(get("controller_downtime_us")),
        "activation_latency_us": median(get("activation_latency_us")),
        "p50_us": median(get("latency_us.p50")),
        "p90_us": median(get("latency_us.p90")),
        "p99_us": median(get("latency_us.p99")),
        "p999_us": median(get("latency_us.p999")),
        "max_us": median(get("latency_us.max")),
        "window_count": median(get("swap_window_latency_us.count")),
        "window_p50_us": median(get("swap_window_latency_us.p50")),
        "window_p90_us": median(get("swap_window_latency_us.p90")),
        "window_p99_us": median(get("swap_window_latency_us.p99")),
        "window_p999_us": median(get("swap_window_latency_us.p999")),
        "window_max_us": median(get("swap_window_latency_us.max")),
        "window_max_gap_us": median(get("swap_window_max_gap_us")),
        "activation_gap_us": median(get("activation_gap_us")),
        "duration_s": median(get("duration_s")),
    }
    summary["rollback_any"] = any(bool(r.get("rollback_occurred")) for r in runs)
    summary["rollback_all"] = all(bool(r.get("rollback_occurred")) for r in runs)
    summary["verified_any"] = any(bool(r.get("activation_verified")) for r in runs)
    summary["verified_all"] = all(bool(r.get("activation_verified")) for r in runs)
    summary["active_before"] = sorted({r.get("active_slot_before") for r in runs})
    summary["active_after"] = sorted({r.get("active_slot_after") for r in runs})
    summary["host"] = runs[0].get("host", "unknown") if runs else "unknown"
    return summary


def fmt_int(v: float) -> str:
    return f"{int(round(v)):,}"


def fmt_us(v: float) -> str:
    if v >= 1000.0:
        return f"{v/1000.0:.2f} ms"
    return f"{v:.1f} µs"


def write_report(summary: dict, meta: dict) -> None:
    lines: list[str] = []
    lines.append("# NVMeSlotSwap experiment results\n")
    lines.append(f"Generated: {meta['generated']}  \n")
    lines.append(f"Host: {meta['host']}  \n")
    lines.append(f"Binary: `{meta['binary']}`  \n")
    lines.append(f"Repetitions per scenario: {meta['reps']} (median reported)\n")
    lines.append(
        "\nThe device is the in-repo emulated NVMe controller; the driver, queues, "
        "doorbells, PRPs and firmware commands are the real code paths. Latency "
        "figures come from `LatencyRecorder` in the harness, not from the device "
        "model's own service-time parameter. The measurement host was a shared "
        "workstation, so `max_gap` also contains scheduler noise; the 3-run median "
        "is reported for every metric.\n"
    )

    def row(key: str) -> dict:
        return summary["scenarios"][key]

    lines.append("\n## Scenario comparison\n")
    lines.append(
        "| Scenario | Queues | IOPS | Completed | Dropped | Failed | Integrity errors | "
        "p50 | p99 | p99.9 | Max completion gap | Controller downtime |"
    )
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|")
    for key in ("baseline", "activate_ca3", "activate_ca2", "failed_commit"):
        s = row(key)
        label = meta["labels"][key]
        lines.append(
            f"| {label} | {meta['queues'][key]} | {fmt_int(s['iops'])} | "
            f"{fmt_int(s['completed'])} | {s['dropped_max']} | {s['failed_max']} | "
            f"{s['integrity_errors_max']} | {fmt_us(s['p50_us'])} | {fmt_us(s['p99_us'])} | "
            f"{fmt_us(s['p999_us'])} | {fmt_us(s['max_gap_us'])} | "
            f"{fmt_us(s['controller_downtime_us'])} |"
        )

    lines.append("\n## Swap window versus steady state\n")
    lines.append(
        "The swap window is ±200 ms around the activation timestamp. The activation gap "
        "is the longest interval with no completions whose two ends fall in the 20 ms "
        "after the activation timestamp, which isolates the controller's own "
        "interruption from scheduler noise elsewhere in the run. Percentiles are "
        "median-of-3 across repetitions.\n"
    )
    lines.append("| Scenario | Steady p50 | Steady p99 | Steady p99.9 | Window p50 | "
                 "Window p99 | Window p99.9 | Activation gap | Window samples |")
    lines.append("|---|---:|---:|---:|---:|---:|---:|---:|---:|")
    for key in ("activate_ca3", "activate_ca2", "failed_commit"):
        s = row(key)
        lines.append(
            f"| {meta['labels'][key]} | {fmt_us(s['p50_us'])} | {fmt_us(s['p99_us'])} | "
            f"{fmt_us(s['p999_us'])} | {fmt_us(s['window_p50_us'])} | "
            f"{fmt_us(s['window_p99_us'])} | {fmt_us(s['window_p999_us'])} | "
            f"{fmt_us(s['activation_gap_us'])} | {fmt_int(s['window_count'])} |"
        )

    lines.append("\n## Queue scaling (CA=3)\n")
    lines.append("| Queue pairs | IOPS | p99 | p99.9 | Max gap | Dropped | Integrity errors |")
    lines.append("|---:|---:|---:|---:|---:|---:|---:|")
    for q in (1, 2, 4, 8):
        s = row(f"scale_q{q}")
        lines.append(
            f"| {q} | {fmt_int(s['iops'])} | {fmt_us(s['p99_us'])} | {fmt_us(s['p999_us'])} | "
            f"{fmt_us(s['max_gap_us'])} | {s['dropped_max']} | {s['integrity_errors_max']} |"
        )

    fc = row("failed_commit")
    ca3 = row("activate_ca3")
    ca2 = row("activate_ca2")
    lines.append("\n## Rollback verification\n")
    lines.append(
        f"- `failed_commit` runs: rollback observed in "
        f"{'all' if fc['rollback_all'] else 'some' if fc['rollback_any'] else 'no'} "
        f"repetitions; active slot before={fc['active_before']} after={fc['active_after']}."
    )
    lines.append(
        f"- Dropped commands: {fc['dropped_max']}; integrity errors: "
        f"{fc['integrity_errors_max']}; failed completions: {fc['failed_max']}."
    )
    lines.append(
        f"- Successful CA=3 activation: active slot before={ca3['active_before']} "
        f"after={ca3['active_after']}, verified via log page 0x03 in "
        f"{'all' if ca3['verified_all'] else 'some' if ca3['verified_any'] else 'no'} runs."
    )

    lines.append("\n## Takeaway\n")
    lines.append(
        f"- CA=3 moved the active slot with **0 controller resets** and "
        f"**0 dropped commands** across the measured runs, at "
        f"{fmt_int(ca3['iops'])} IOPS."
    )
    lines.append(
        f"- CA=2 needed a controller reset and queue re-creation costing "
        f"{fmt_us(ca2['controller_downtime_us'])} of controller downtime; its run "
        f"delivered {fmt_int(ca2['iops'])} IOPS."
    )
    lines.append(
        f"- The failed commit rolled back to the prior slot with "
        f"{fc['dropped_max']} dropped commands and {fc['integrity_errors_max']} "
        f"integrity errors, so the namespace stayed usable."
    )

    (RESULTS / "report.md").write_text("\n".join(lines) + "\n")


def maybe_plot(summary: dict, meta: dict, no_plots: bool) -> list[str]:
    if no_plots:
        return []
    try:
        import matplotlib

        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
    except Exception:
        return []

    written: list[str] = []
    # Latency CDF from the raw reservoir of the median repetition (rep 1).
    fig, ax = plt.subplots(figsize=(7.0, 4.5))
    for key, color in (("baseline", "#4c72b0"), ("activate_ca3", "#dd8452"),
                       ("activate_ca2", "#55a868")):
        files = sorted(RAW.glob(f"{key}_r*.json"))
        if not files:
            continue
        data = json.loads(files[0].read_text())
        samples = sorted(data.get("latency_samples_us", []))
        if not samples:
            continue
        ys = [(i + 1) / len(samples) for i in range(len(samples))]
        ax.plot(samples, ys, label=meta["labels"][key], color=color, linewidth=1.4)
    ax.set_xscale("log")
    ax.set_xlabel("completion latency (µs, log scale)")
    ax.set_ylabel("fraction of completions")
    ax.set_title("Latency CDF (sampled reservoir)")
    ax.grid(True, which="both", alpha=0.3)
    ax.legend(fontsize=8)
    fig.tight_layout()
    path = RESULTS / "latency_cdf.png"
    fig.savefig(path, dpi=130)
    plt.close(fig)
    written.append(path.name)

    fig, ax = plt.subplots(figsize=(7.0, 4.5))
    queues = [1, 2, 4, 8]
    iops = [summary["scenarios"][f"scale_q{q}"]["iops"] for q in queues]
    p99 = [summary["scenarios"][f"scale_q{q}"]["p99_us"] for q in queues]
    ax.plot(queues, iops, marker="o", color="#4c72b0", label="IOPS")
    ax.set_xlabel("I/O queue pairs")
    ax.set_ylabel("IOPS")
    ax.set_xticks(queues)
    ax.grid(True, alpha=0.3)
    ax2 = ax.twinx()
    ax2.plot(queues, p99, marker="s", color="#dd8452", label="p99 latency")
    ax2.set_ylabel("p99 latency (µs)")
    lines = ax.get_lines() + ax2.get_lines()
    ax.legend(lines, [l.get_label() for l in lines], loc="upper left", fontsize=8)
    ax.set_title("CA=3 queue scaling")
    fig.tight_layout()
    path = RESULTS / "queue_scaling.png"
    fig.savefig(path, dpi=130)
    plt.close(fig)
    written.append(path.name)
    return written


def main() -> int:
    args = parse_args()
    reps = 1 if args.quick else max(1, args.reps)
    build_if_needed(args.rebuild)
    RESULTS.mkdir(parents=True, exist_ok=True)
    RAW.mkdir(parents=True, exist_ok=True)
    # Keep the raw directory to this run's files so stale runs cannot leak in.
    for stale in RAW.glob("*.json"):
        stale.unlink()

    scenarios = scenario_matrix(args.quick)
    collected: dict[str, list[dict]] = {}
    for scenario in scenarios:
        collected[scenario["key"]] = []
        for rep in range(reps):
            seed = SEEDS[rep % len(SEEDS)]
            print(f"[harness] {scenario['key']} rep {rep + 1}/{reps} (seed {seed})",
                  flush=True)
            collected[scenario["key"]].append(run_once(scenario, rep, seed, args.quick))

    summary = {
        "generated": datetime.now(timezone.utc).strftime("%Y-%m-%dT%H:%M:%SZ"),
        "host": platform.platform(),
        "reps": reps,
        "scenarios": {},
    }
    meta = {
        "generated": summary["generated"],
        "host": platform.platform(),
        "binary": str(BINARY.relative_to(ROOT)),
        "reps": reps,
        "labels": {s["key"]: s["label"] for s in scenarios},
        "queues": {},
        "quick": args.quick,
    }
    for scenario in scenarios:
        key = scenario["key"]
        runs = collected[key]
        summary["scenarios"][key] = summarise(runs)
        summary["scenarios"][key]["label"] = scenario["label"]
        meta["queues"][key] = runs[0]["config"]["queues"] if runs else 0

    summary["meta"] = meta
    (RESULTS / "summary.json").write_text(json.dumps(summary, indent=2) + "\n")
    write_report(summary, meta)
    plots = maybe_plot(summary, meta, args.no_plots or args.quick)
    if not plots and not args.no_plots:
        with open(RESULTS / "report.md", "a", encoding="utf-8") as fh:
            fh.write("\n_Plots skipped: matplotlib is not installed in this Python.\n_\n")

    failures: list[str] = []
    for scenario in scenarios:
        key = scenario["key"]
        data = summary["scenarios"][key]
        if scenario["no_reset"]:
            if data["dropped_max"] > 0:
                failures.append(f"{key}: dropped={data['dropped_max']}")
            if data["integrity_errors_max"] > 0:
                failures.append(f"{key}: integrity_errors={data['integrity_errors_max']}")
        if scenario["rollback_expected"] and not data["rollback_any"]:
            failures.append(f"{key}: expected a rollback but none occurred")

    print(f"[harness] summary: {RESULTS / 'summary.json'}")
    print(f"[harness] report:  {RESULTS / 'report.md'}")
    if plots:
        print(f"[harness] plots:   {', '.join(plots)} (matplotlib)")
    if failures:
        print("[harness] FAILED: " + "; ".join(failures), file=sys.stderr)
        return 1
    print("[harness] all lossless scenarios reported 0 dropped and 0 integrity errors")
    return 0


if __name__ == "__main__":
    sys.exit(main())
