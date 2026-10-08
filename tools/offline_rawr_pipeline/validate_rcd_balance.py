"""Validate production balanced RCD's camera-linear contract on all four Bayer patterns.
Run with uv run --with numpy; arguments: RUNNER WORKDIR.
"""

from pathlib import Path
import subprocess
import sys
import numpy as np

runner = Path(sys.argv[1]).resolve()
root = Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)
patterns = [[[0, 1], [1, 2]], [[1, 0], [2, 1]], [[1, 2], [0, 1]], [[2, 1], [1, 0]]]
for pattern, layout in enumerate(patterns):
    for kind in ["constant", "detail", "black", "clipped"]:
        w, h = 66, 50
        packed = np.empty((h // 2, w // 2, 4), np.float16)
        packed[:] = [0.13, 0.38, 0.38, 0.24]
        if kind == "black":
            packed[:] = 0
        if kind == "clipped":
            packed[:] = 1.2
        if kind == "detail":
            yy, xx = np.mgrid[: h // 2, : w // 2]
            factor = 0.25 + 0.75 * ((xx + yy) % 5) / 4
            packed *= factor[..., None]
        name = f"{pattern}_{kind}"
        inp = root / (name + ".rgba16f")
        packed.tofile(inp)
        cfg = root / (name + ".txt")
        cfg.write_text(
            f"width={w}\nheight={h}\ncfa={pattern}\nfccSteps=0\ndefringeStrength=0\nwbRgb=1,1,1\n"
        )
        out = root / name
        p = subprocess.run(
            [
                str(runner),
                "--input",
                str(inp),
                "--config",
                str(cfg),
                "--output-dir",
                str(out),
                "--rcd",
                "--dump-demosaic",
                "--single-recovery",
                "0",
            ],
            capture_output=True,
            text=True,
            check=True,
        )
        assert "VARIANT_FAIL" not in p.stdout, p.stdout
        y = np.fromfile(out / "demosaiced_camera_rgb.rgba16f", np.float16).reshape(
            h, w, 4
        )
        assert np.isfinite(y).all()
        assert (y[..., 3] == 1).all()
        for row in range(2):
            for col in range(2):
                channel = layout[row][col]
                packedchannel = [0, 1, 3][channel]
                e = abs(
                    y[row::2, col::2, channel].astype(float)
                    - packed[..., packedchannel].astype(float)
                )
                assert e.max() < 0.002, (name, row, col, e.max())
        if kind != "detail":
            expected = packed[0, 0, [0, 1, 3]]
            assert abs(y[..., :3].astype(float) - expected).max() < 0.002, name
        # At zero dual contrast, the RCD branch must match standalone RCD exactly.
        # This catches a caller failing to enable the shared balance policy.
        settings = cfg.read_text()
        for mode, contrast in [("rcd_only", 0), ("blend", 20)]:
            cfg.write_text(
                settings + f"dualAutoContrast=0\ndualContrastPercent={contrast}\n"
            )
            dual_out = root / (name + "_dual_" + mode)
            command = list(p.args)
            command.remove("--rcd")
            command[command.index("--output-dir") + 1] = str(dual_out)
            result = subprocess.run(command, capture_output=True, text=True, check=True)
            assert "VARIANT_FAIL" not in result.stdout, result.stdout
            dual_y = np.fromfile(
                dual_out / "demosaiced_camera_rgb.rgba16f", np.float16
            ).reshape(h, w, 4)
            assert np.isfinite(dual_y).all(), (name, mode)
            assert (dual_y[..., 3] == 1).all(), (name, mode, "alpha")
            if contrast == 0:
                assert np.array_equal(y, dual_y), (name, "standalone/dual RCD mismatch")
        cfg.write_text(settings)
print(
    "PASS RCD all CFA patterns, partial workgroups, native samples, camera-linear scale, "
    "black/clipped fallback, finite/alpha; dual RCD bit-exact parity and finite blended output"
)
