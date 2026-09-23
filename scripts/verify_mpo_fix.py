#!/usr/bin/env python3
"""
Field verification for the MPO/DXGI interleaving fix.

Usage:
    python verify_mpo_fix.py <output_dir>

Reads every stutto_report_*.json in <output_dir> and prints a pass/fail verdict
based on baseline plausibility, spike-ratio internal consistency, and frame
timeline unimodality.
"""
import json
import statistics
import sys
from pathlib import Path


def check_report(name, report):
    flags = []
    t = report.get("trigger", {})
    baseline_ms = t.get("baseline_avg_ms", 0.0)
    baseline_fps = t.get("baseline_fps", 0.0)
    duration_ms = t.get("duration_ms", 0.0)
    spike_ratio = t.get("spike_ratio", 0.0)

    if baseline_ms < 2.0:
        flags.append(f"baseline_avg_ms={baseline_ms:.3f} is implausibly low")
    if baseline_fps > 500.0:
        flags.append(f"baseline_fps={baseline_fps:.1f} exceeds physical limits")
    if baseline_ms > 0 and duration_ms > 0:
        expected = duration_ms / baseline_ms
        if abs(spike_ratio - expected) > 0.05 * expected:
            flags.append(
                f"spike_ratio={spike_ratio:.2f} inconsistent with "
                f"duration/baseline={expected:.2f}"
            )

    timeline = report.get("frame_timeline", [])
    if len(timeline) >= 32:
        durs = sorted(pt["duration_ms"] for pt in timeline)
        gaps = [durs[i + 1] - durs[i] for i in range(len(durs) - 1)]
        max_gap = max(gaps) if gaps else 0.0
        median_dur = statistics.median(durs) if durs else 0.0
        if median_dur > 0 and max_gap > 3.0 * median_dur:
            flags.append(
                f"frame_timeline is bimodal (max_gap={max_gap:.2f}ms, "
                f"median={median_dur:.2f}ms) — symptom of interleaving bug"
            )
    return flags


def main(output_dir):
    reports = []
    for f in sorted(Path(output_dir).glob("stutto_report_*.json")):
        try:
            with open(f) as fp:
                reports.append((f.name, json.load(fp)))
        except Exception as e:
            print(f"WARNING: failed to parse {f.name}: {e}")

    if not reports:
        print("No reports found in", output_dir)
        print("Either the game was too smooth, or --target-process did not match.")
        return 2

    print(f"Analyzed {len(reports)} report(s)\n")

    baselines = [r["trigger"].get("baseline_avg_ms", 0.0) for _, r in reports]
    fps_vals = [r["trigger"].get("baseline_fps", 0.0) for _, r in reports]
    spikes = [r["trigger"].get("spike_ratio", 0.0) for _, r in reports]

    print(f"baseline_avg_ms: min={min(baselines):8.3f}  "
          f"median={statistics.median(baselines):8.3f}  max={max(baselines):8.3f}")
    print(f"baseline_fps:    min={min(fps_vals):8.1f}  "
          f"median={statistics.median(fps_vals):8.1f}  max={max(fps_vals):8.1f}")
    print(f"spike_ratio:     min={min(spikes):8.2f}  "
          f"median={statistics.median(spikes):8.2f}  max={max(spikes):8.2f}")
    print()

    total_flags = 0
    for name, report in reports:
        flags = check_report(name, report)
        if flags:
            print(f"[FLAG] {name}")
            for flag in flags:
                print(f"       - {flag}")
            total_flags += len(flags)

    print()
    if total_flags == 0:
        print("VERDICT: PASS — no red flags across all reports.")
        return 0
    else:
        print(f"VERDICT: FAIL — {total_flags} red flag(s) across {len(reports)} reports.")
        return 1


if __name__ == "__main__":
    if len(sys.argv) != 2:
        print(__doc__)
        sys.exit(64)
    sys.exit(main(sys.argv[1]))
