#!/usr/bin/env python3
"""Compare repeated, same-device live scope runs (use uv run --no-project).

Require >=10% typical improvement at 25% sampling and reject >5% median/p95
regressions that recur in every paired run. Transient regressions are reported.
Reverse baseline/candidate order between pairs to reduce thermal/order bias.
"""
import argparse
from pathlib import Path
import re
import statistics


def read(path):
    text = Path(path).read_text()
    if "IMAGE_SCOPES_LIVE_BENCHMARK_PASS" not in text:
        raise ValueError(f"{path}: incomplete live benchmark")
    identity = re.search(r"^DEVICE .+$", text, re.M)
    memory = re.search(r"^LIVE_MEMORY .+$", text, re.M)
    if not identity or not memory:
        raise ValueError(f"{path}: missing device/memory metadata")
    timings = {
        name: (float(median), float(p95))
        for name, median, p95 in re.findall(
            r"^(live_\S+) .*?median=([\d.]+).*?p95=([\d.]+)", text, re.M
        )
    }
    pixels = dict(re.findall(r"^PIXELS (\S+) (\d+)$", text, re.M))
    if not timings or timings.keys() != pixels.keys():
        raise ValueError(f"{path}: incomplete timing/pixel pairs")
    if any(min(values) <= 0 for values in timings.values()):
        raise ValueError(f"{path}: invalid timing")
    return identity.group(), memory.group(), timings, pixels


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--baseline", nargs="+", required=True)
    parser.add_argument("--candidate", nargs="+", required=True)
    parser.add_argument("--target", choices=("vectorscope", "waveform"), default="vectorscope")
    args = parser.parse_args()
    if len(args.baseline) != len(args.candidate) or len(args.baseline) < 2:
        parser.error("provide at least two paired baseline/candidate runs")
    baselines = [read(p) for p in args.baseline]
    candidates = [read(p) for p in args.candidate]
    identity, memory, reference, pixels = baselines[0]
    for dev, mem, timing, hashes in baselines + candidates:
        if dev != identity or mem != memory or timing.keys() != reference.keys():
            raise ValueError("device, memory, or workload matrix differs between runs")
        if hashes != pixels:
            raise ValueError("rendered pixel fingerprints differ between runs")
    failed = False
    targeted = []
    transient = 0
    for name in sorted(reference):
        for index, metric in enumerate(("median", "p95")):
            ratios = [c[2][name][index] / b[2][name][index] for b, c in zip(baselines, candidates)]
            if min(ratios) > 1.05:
                print(f"REGRESSION {name} {metric}: " + ", ".join(f"{r:.3f}x" for r in ratios))
                failed = True
            elif max(ratios) > 1.05:
                transient += 1
            is_target = ("_vec" in name) == (args.target == "vectorscope")
            if index == 0 and "_25" in name and is_target:
                targeted.append(statistics.median(ratios))
    if not targeted:
        raise ValueError("no production 25% workloads")
    improvement = 1 - statistics.median(targeted)
    print(f"{len(reference)} workloads, {len(baselines)} pairs; pixel fingerprints match")
    print(f"Typical 25% GPU median improvement: {improvement:.1%}")
    print(f"Transient >5% timing regressions: {transient} (not repeated in every pair)")
    if improvement < .10:
        print("FAIL: typical 25% improvement below 10%")
        failed = True
    print("LIVE_COMPARISON_FAIL" if failed else "LIVE_COMPARISON_PASS")
    return int(failed)


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except ValueError as error:
        raise SystemExit(f"LIVE_COMPARISON_FAIL: {error}") from error
