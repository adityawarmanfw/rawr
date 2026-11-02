#!/usr/bin/env python3
"""Compare headless cold-replay reports; fail on recipe/pixel/size differences."""
import argparse
import json
import statistics
from pathlib import Path


def summarize(path: Path) -> dict:
    rows = json.loads(path.read_text())
    if not rows:
        raise ValueError(f"Empty replay report: {path}")
    recipes = {json.dumps(row["recipe"], sort_keys=True) for row in rows}
    hashes = {row["decodedArgbSha256"] for row in rows}
    sizes = {row["jpegBytes"] for row in rows}
    if len(recipes) != 1 or len(hashes) != 1 or len(sizes) != 1:
        raise ValueError(f"Unstable recipe, decoded pixels or JPEG size: {path}")
    return {
        "samples": len(rows),
        "recipe": json.loads(recipes.pop()),
        "decodedArgbSha256": hashes.pop(),
        "jpegBytes": sizes.pop(),
        "medianOpenRenderFsyncCloseMs": statistics.median(row["openRenderFsyncCloseMs"] for row in rows),
        "medianPostClosePssKiB": statistics.median(row["postClosePssKiB"] for row in rows),
    }


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("before", type=Path)
    parser.add_argument("after", type=Path)
    args = parser.parse_args()
    before, after = summarize(args.before), summarize(args.after)
    for field in ("recipe", "decodedArgbSha256", "jpegBytes"):
        if before[field] != after[field]:
            raise SystemExit(f"FAIL: {field} differs")
    print(json.dumps({
        "decodedPixelsExact": True,
        "before": before,
        "after": after,
        "caveat": "Small samples under competing workload; PSS is after close, not peak. Not capture-to-gallery latency or power.",
    }, indent=2))


if __name__ == "__main__":
    main()
