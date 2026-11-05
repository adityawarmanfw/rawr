"""Real Activity/Home/resume checks through scripts/ui. Run with uv."""
import argparse
import csv
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
    parser.add_argument("--cycles", type=int, default=3)
    parser.add_argument("--gesture-home", action="store_true")
    args = parser.parse_args()
    if not 1 <= args.cycles <= 10:
        parser.error("cycles must be 1–10")
    args.output.mkdir(parents=True, exist_ok=False)
    package = "com.rawr.camera.benchmark"
    env = dict(os.environ, UI_PKG=package)

    def adb(*parts):
        if parts and parts[0] == "shell":
            parts = ("shell", shlex.join(parts[1:]))
        return subprocess.run(["adb", *parts], check=True, capture_output=True, text=True).stdout

    def ui(*parts):
        return subprocess.run([str(ROOT / "scripts/ui"), *parts], cwd=ROOT, env=env,
                              check=True, capture_output=True, text=True).stdout

    def memory(label):
        report = adb("shell", "dumpsys", "meminfo", package)
        (args.output / f"{label}-memory.txt").write_text(report)
        match = re.search(r"TOTAL PSS:\s+(\d+)", report)
        if not match:
            raise RuntimeError("App process exited or memory report is unavailable")
        return int(match[1])

    rows = []
    try:
        for cycle in range(args.cycles):
            begin = time.monotonic()
            ui("app")
            ready_ms = (time.monotonic() - begin) * 1000
            time.sleep(2)
            pid = adb("shell", "pidof", package).strip()
            foreground = memory(f"cycle-{cycle}-foreground")
            surface_fps = None
            layers = adb("shell", "dumpsys", "SurfaceFlinger", "--list").splitlines()
            candidates = [s for s in layers if package in s and "SurfaceView" in s and "BLAST" in s]
            for surface in candidates[:3]:
                try:
                    latency = adb("shell", "dumpsys", "SurfaceFlinger", "--latency", surface)
                except subprocess.CalledProcessError as error:
                    # Layer latency is optional and unavailable on some OS
                    # builds. Keep the core lifecycle/camera checks running.
                    (args.output / f"cycle-{cycle}-surface-error.txt").write_text(error.stderr or str(error))
                    continue
                timestamps = []
                for line in latency.splitlines():
                    parts = line.split()
                    if len(parts) == 3 and all(p.isdigit() for p in parts):
                        stamp = int(parts[1])
                        if 0 < stamp < 2**63 - 1:
                            timestamps.append(stamp)
                timestamps = sorted(set(timestamps))
                if len(timestamps) >= 3:
                    (args.output / f"cycle-{cycle}-surface-latency.txt").write_text(latency)
                    surface_fps = (len(timestamps) - 1) * 1e9 / (timestamps[-1] - timestamps[0])
                    break
            (args.output / f"cycle-{cycle}-gfx.txt").write_text(adb("shell", "dumpsys", "gfxinfo", package))
            adb("shell", "dumpsys", "gfxinfo", "com.miui.home", "reset")
            marker = f"home-{cycle}-{time.monotonic_ns()}"
            adb("shell", "log", "-p", "i", "-t", "RawrHomeTiming", marker)
            ui("home", *(["--gesture"] if args.gesture_home else []))
            time.sleep(4)
            launcher = adb("shell", "dumpsys", "gfxinfo", "com.miui.home", "framestats")
            (args.output / f"cycle-{cycle}-launcher-gfx.txt").write_text(launcher)
            background = memory(f"cycle-{cycle}-home")
            if adb("shell", "pidof", package).strip() != pid:
                raise RuntimeError("Process changed across Home; cannot compare retained memory")
            camera = adb("shell", "dumpsys", "media.camera")
            (args.output / f"cycle-{cycle}-camera.txt").write_text(camera)
            active = camera.split("Active Camera Clients:", 1)[1].split("Allowed user IDs:", 1)[0]
            if package in active:
                raise RuntimeError("Camera remains active four seconds after Home")
            row = dict(cycle=cycle, foregroundPssKiB=foreground, homePssKiB=background,
                       uiReadyObservationMs=ready_ms, cameraClosed=True, sameProcess=True)
            if surface_fps is not None:
                row["observedSurfaceFps"] = surface_fps
            jank = re.search(r"Janky frames:\s+(\d+)\s+\(([\d.]+)%\)", launcher)
            if jank:
                row.update(launcherJankyFrames=int(jank[1]), launcherJankyPercent=float(jank[2]))
            markers = adb("logcat", "-d", "-v", "monotonic", "-s", "RawrHomeTiming:I", "*:S")
            stamp = next((re.match(r"\s*([\d.]+)", line) for line in markers.splitlines() if marker in line), None)
            if stamp:
                start_ns = int(float(stamp[1]) * 1e9)
                animation = []
                for section in launcher.split("---PROFILEDATA---"):
                    lines = section.strip().splitlines()
                    if not lines or not lines[0].startswith("Flags,"):
                        continue
                    for frame in csv.DictReader(lines):
                        intended = int(frame.get("IntendedVsync", 0))
                        completed = int(frame.get("FrameCompleted", 0))
                        deadline = int(frame.get("FrameDeadline", 0))
                        if (int(frame.get("Flags", 1)) == 0 and start_ns <= intended < start_ns + 1_500_000_000
                                and intended < completed < 2**63 - 1 and deadline > intended):
                            animation.append(completed > deadline)
                row.update(gestureWindowFrames=len(animation), gestureWindowMissedDeadlines=sum(animation))
            rows.append(row)
            (args.output / "results.json").write_text(json.dumps(rows, indent=2))
            print(json.dumps(row), flush=True)
        ui("app")
        (args.output / "resumed-ui.txt").write_text(ui("state"))
        (args.output / "summary.json").write_text(json.dumps(dict(cycles=len(rows), passed=True,
            note="UI-ready time includes scripts/ui polling; not Home animation or sensor-to-display latency."), indent=2))
    finally:
        ui("stop")


if __name__ == "__main__":
    main()
