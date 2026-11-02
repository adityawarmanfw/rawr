"""Validate and summarize run-device-native.sh results. Standard library only.

uv run --no-project tools/phone-performance/summarize.py tmp/phone-performance/device-results
"""
from __future__ import annotations

import hashlib
import json
import math
from pathlib import Path
import statistics
import sys


def summarize(root: Path) -> str:
    lines = [
        "CPU JPEG benchmark: three warmups and 20 samples per process, ABBA order.",
        "Times include encoding and output to /dev/null; file save is outside timing.",
        "RSS is the range of per-process high-water marks; it is not total app memory.",
        "",
        "| Input | Variant | Samples | Median ms | p95 ms | Peak RSS MiB |",
        "| --- | --- | ---: | ---: | ---: | ---: |",
    ]
    hashes: list[str] = []
    for case in ("12mp", "50mp"):
        samples: dict[str, list[float]] = {"baseline": [], "candidate": []}
        peaks: dict[str, list[float]] = {"baseline": [], "candidate": []}
        sizes: set[int] = set()
        for round_, variant in enumerate(("baseline", "candidate", "candidate", "baseline"), 1):
            stem = root / f"{case}-{round_}-{variant}"
            summary = json.loads(stem.with_suffix(".json").read_text())
            raw = [json.loads(line) for line in Path(f"{stem}-samples.jsonl").read_text().splitlines()]
            if summary["samples"] != 20 or summary["warmups"] != 3 or len(raw) != 20:
                raise ValueError(f"{stem}: incomplete sample set")
            expected_mode = "flat" if variant == "baseline" else "segments"
            if summary["mode"] != expected_mode or summary["quality"] != 98 or summary["subsampling"] != 1:
                raise ValueError(f"{stem}: unexpected recipe or encoder variant")
            if (summary["width"], summary["height"]) != ((4080, 3064) if case == "12mp" else (8160, 6128)):
                raise ValueError(f"{stem}: unexpected dimensions")
            values = [row["totalMs"] for row in raw]
            if [row["sample"] for row in raw] != list(range(20)) or any(not math.isfinite(v) or v < 0 for v in values):
                raise ValueError(f"{stem}: invalid raw samples")
            if abs(statistics.median(values) - summary["totalMedianMs"]) > 0.002:
                raise ValueError(f"{stem}: summary median disagrees with raw samples")
            samples[variant].extend(values)
            if summary["peakRssBytes"] <= 0:
                raise ValueError(f"{stem}: RSS not available")
            peaks[variant].append(summary["peakRssBytes"] / 1048576)
            sizes.add(summary["jpegBytes"])
        for variant in ("baseline", "candidate"):
            values = sorted(samples[variant])
            rss = peaks[variant]
            lines.append(
                f"| {case} | {variant} | {len(values)} | {statistics.median(values):.2f} | "
                f"{values[math.ceil(0.95 * len(values)) - 1]:.2f} | {min(rss):.2f}–{max(rss):.2f} |"
            )
        baseline = (root / f"{case}-baseline.jpg").read_bytes()
        candidate = (root / f"{case}-candidate.jpg").read_bytes()
        if baseline != candidate or sizes != {len(baseline)}:
            raise ValueError(f"{case}: baseline/candidate JPEGs differ or byte count is wrong")
        digest = hashlib.sha256(baseline).hexdigest()
        hashes.append(f"{case}: {len(baseline):,} identical bytes; SHA-256 `{digest}`.")
    return "\n".join(lines + [""] + hashes) + "\n"


if __name__ == "__main__":
    if len(sys.argv) != 2:
        raise SystemExit("Usage: uv run --no-project tools/phone-performance/summarize.py RESULTS_DIR")
    try:
        print(summarize(Path(sys.argv[1])), end="")
    except (OSError, ValueError, KeyError) as error:
        raise SystemExit(f"Benchmark results rejected: {error}") from error
