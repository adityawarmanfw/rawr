"""Collect live production viewfinder telemetry without changing image settings."""
import argparse
import json
import os
from pathlib import Path
import re
import shlex
import subprocess
import time

ROOT = Path(__file__).resolve().parents[2]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--seconds", type=int, default=25)
    args = parser.parse_args()
    if not 15 <= args.seconds <= 120:
        parser.error("seconds must be 15–120")
    args.output.mkdir(parents=True, exist_ok=False)
    package = "com.rawr.camera.benchmark"
    env = dict(os.environ, UI_PKG=package)

    def shell(*parts):
        return subprocess.run(["adb", "shell", shlex.join(parts)], check=True, capture_output=True, text=True).stdout

    def ui(*parts):
        return subprocess.run([str(ROOT / "scripts/ui"), *parts], cwd=ROOT, env=env,
                              check=True, capture_output=True, text=True).stdout

    log = None
    process = None
    try:
        ui("stop")
        ui("app")
        (args.output / "ui-before.txt").write_text(ui("state"))
        pid = shell("pidof", package).strip()
        if not pid.isdigit():
            raise RuntimeError("App did not start")
        (args.output / "thermal-before.txt").write_text(shell("dumpsys", "thermalservice"))
        (args.output / "memory-before.txt").write_text(shell("dumpsys", "meminfo", package))
        log = (args.output / "logcat.txt").open("w")
        process = subprocess.Popen(["adb", "logcat", "--pid", pid, "-T", "1", "-v", "monotonic"], stdout=log, stderr=subprocess.STDOUT)
        time.sleep(args.seconds)
        if shell("pidof", package).strip() != pid:
            raise RuntimeError("App process exited during preview measurement")
        process.terminate(); process.wait(timeout=5); process = None
        log.close(); log = None
        (args.output / "gfx.txt").write_text(shell("dumpsys", "gfxinfo", package))
        (args.output / "memory-after.txt").write_text(shell("dumpsys", "meminfo", package))
        rows = []
        for line in (args.output / "logcat.txt").read_text().splitlines():
            if "PREVIEW_GPU_BREAKDOWN" in line:
                rows.append({key: float(value) for key, value in re.findall(r"(\w+)=([\d.]+)(?:ms)?", line)})
        if not rows:
            raise RuntimeError("No completed GPU telemetry interval; increase duration")
        report = dict(seconds=args.seconds, intervals=rows,
                      note="Native GPU timestamps; UI gfx counters do not measure sensor-to-display latency. Preserve recipe/scene for A/B.")
        (args.output / "summary.json").write_text(json.dumps(report, indent=2))
        print(json.dumps(report), flush=True)
    finally:
        if process is not None:
            process.terminate(); process.wait(timeout=5)
        if log is not None:
            log.close()
        ui("stop")


if __name__ == "__main__":
    main()
