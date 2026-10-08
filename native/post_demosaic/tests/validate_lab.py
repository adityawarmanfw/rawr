"""uv run --with numpy validate_lab.py RUNNER WORKDIR

Exercise the production Vulkan component: tiny/odd geometry, matrix changes,
bypass, HDR, alpha, repeated reuse, and brightness preservation under repair.
Temporary work stays in the explicitly supplied directory (use ./tmp).
"""

import json
from pathlib import Path
import subprocess
import sys
import numpy as np

runner, root = Path(sys.argv[1]).resolve(), Path(sys.argv[2])
root.mkdir(parents=True, exist_ok=True)
matrices = [
    np.eye(3),
    np.array([[1.38, -0.249, -0.118], [-0.181, 1.468, -0.291], [0.055, -0.795, 1.746]]),
]
xyz = np.array(
    [
        [0.4360747, 0.3850649, 0.1430804],
        [0.2225045, 0.7168786, 0.0606169],
        [0.0139322, 0.0971045, 0.7141733],
    ]
)
results = []


def run(x, matrix, name, strength=1, iterations=1):
    source = root / (name + ".input")
    target = root / (name + ".output")
    mp = root / (name + ".matrix")
    x.astype(np.float16).tofile(source)
    np.savetxt(mp, matrix)
    p = subprocess.run(
        [
            str(runner),
            str(source),
            str(target),
            str(x.shape[1]),
            str(x.shape[0]),
            str(mp),
            str(strength),
            str(iterations),
        ],
        capture_output=True,
        text=True,
        check=True,
    )
    y = np.fromfile(target, np.float16).reshape(x.shape)
    assert np.isfinite(y).all(), name
    assert np.array_equal(y[..., 3], x[..., 3]), name + " alpha"
    return y


for index, matrix in enumerate(matrices):
    for h, w in [(1, 1), (29, 33), (48, 64)]:
        x = np.full((h, w, 4), 0.15, np.float16)
        x[..., 3] = np.arange(h * w).reshape(h, w) % 4 / 4
        assert np.array_equal(run(x, matrix, f"constant_{index}_{w}"), x)
        if w == 1:
            continue
        x[:, : w // 2, :3] = 0.8
        x[:, w // 2 : w // 2 + 2, :3] = [0.3, 0.02, 0.3]
        assert np.array_equal(run(x, matrix, f"bypass_{index}_{w}", 0), x)
        y = run(x, matrix, f"active_{index}_{w}")
        again = run(x, matrix, f"reuse_{index}_{w}", iterations=2)
        assert np.array_equal(y, again), "frame reuse"
        changed = np.any(y[..., :3] != x[..., :3], 2)
        assert changed.any(), "no repair exercised"
        luminance = (xyz @ matrix)[1]
        delta = abs((y[..., :3].astype(float) - x[..., :3].astype(float)) @ luminance)
        assert delta.max() < 0.001, "brightness drift"
        results.append(
            {
                "matrix": index,
                "size": [w, h],
                "changed": int(changed.sum()),
                "max_Y_delta": float(delta.max()),
            }
        )
        hdr = x.copy()
        hdr[..., :3] *= 10000
        run(hdr, matrix, f"hdr_{index}_{w}")
# Singular calibration must fail explicitly.
x = np.ones((1, 1, 4), np.float16)
try:
    run(x, np.zeros((3, 3)), "singular")
except subprocess.CalledProcessError:
    pass
else:
    raise AssertionError("singular calibration accepted")
(root / "results.json").write_text(json.dumps(results, indent=2))
print(
    "PASS tiny/odd/constant/bypass/alpha/matrix/HDR/reuse/brightness; invalid calibration rejected"
)
