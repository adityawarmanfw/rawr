# /// script
# requires-python = ">=3.10"
# dependencies = ["tifffile>=2024.1"]
# ///
"""Test DNG camera profiles (.dcp) built from a rawr DNG's own colour tags.

    uv run tools/dcp/make_test_dcps.py capture.dng tmp/dcp

Writes, next to each other:

  roundtrip.dcp      The DNG's matrices as a profile. Renders the same as no
                     profile; any shift is a pipeline bug. CameraCalibration is
                     folded into ColorMatrix (exact for the diagonal ones phones
                     report).
  roundtrip_be.dcp   The same, big-endian ("MMCR"), for the parser.
  single.dcp         ColorMatrix1 only: single illuminant, derived ForwardMatrix.
  hue30.dcp          + HueSatMap rotating every hue by +30 degrees.
  split.dcp          + HueSatMap1 -40 degrees / HueSatMap2 +40 degrees: the
                     shift follows the light (illuminant interpolation).
  gray_look.dcp      + LookTable with saturation x0: grayscale render.
  tone_s.dcp         + strong S ProfileToneCurve: DNG editors only.
"""

import math
import struct
import sys
from pathlib import Path

import tifffile

COLOR_MATRIX1, COLOR_MATRIX2 = 50721, 50722
CAMERA_CALIBRATION1, CAMERA_CALIBRATION2 = 50723, 50724
ILLUMINANT1, ILLUMINANT2 = 50778, 50779
FORWARD_MATRIX1, FORWARD_MATRIX2 = 50964, 50965
UNIQUE_CAMERA_MODEL = 50708
PROFILE_NAME = 50936
HUE_SAT_DIMS, HUE_SAT_DATA1, HUE_SAT_DATA2 = 50937, 50938, 50939
TONE_CURVE = 50940
EMBED_POLICY = 50941
LOOK_DIMS, LOOK_DATA = 50981, 50982

ASCII, SHORT, LONG, SRATIONAL, FLOAT = 2, 3, 4, 10, 11


def rationals(value):
    """tifffile gives (num, den, num, den, ...) or ((num, den), ...)."""
    flat = []
    for v in value:
        flat.extend(v if isinstance(v, tuple) else [v])
    return [flat[i] / flat[i + 1] for i in range(0, len(flat), 2)]


def matmul(a, b):
    return [sum(a[r * 3 + k] * b[k * 3 + c] for k in range(3)) for r in range(3) for c in range(3)]


def read_dng(path):
    with tifffile.TiffFile(path) as tif:
        tags = {t.code: t.value for page in tif.pages for t in page.tags}
    out = {"illuminant1": int(tags[ILLUMINANT1]), "model": str(tags.get(UNIQUE_CAMERA_MODEL, "rawr"))}
    cm1 = rationals(tags[COLOR_MATRIX1])
    cc1 = rationals(tags[CAMERA_CALIBRATION1]) if CAMERA_CALIBRATION1 in tags else None
    out["cm1"] = matmul(cc1, cm1) if cc1 else cm1
    if FORWARD_MATRIX1 in tags:
        out["fm1"] = rationals(tags[FORWARD_MATRIX1])
    if COLOR_MATRIX2 in tags and ILLUMINANT2 in tags:
        cm2 = rationals(tags[COLOR_MATRIX2])
        cc2 = rationals(tags[CAMERA_CALIBRATION2]) if CAMERA_CALIBRATION2 in tags else None
        out["cm2"] = matmul(cc2, cm2) if cc2 else cm2
        out["illuminant2"] = int(tags[ILLUMINANT2])
        if FORWARD_MATRIX2 in tags and "fm1" in out:
            out["fm2"] = rationals(tags[FORWARD_MATRIX2])
        else:
            out.pop("fm1", None)
    return out


def srational(x):
    den = 1_000_000
    return (int(round(x * den)), den)


def write_dcp(path, tags, big_endian=False):
    """tags: {tag: (type, values)}; a minimal one-IFD DCP."""
    e = ">" if big_endian else "<"
    entries = sorted(tags.items())
    ifd_offset = 8
    data_offset = ifd_offset + 2 + 12 * len(entries) + 4
    directory = struct.pack(e + "H", len(entries))
    payload = b""
    for tag, (typ, values) in entries:
        if typ == ASCII:
            raw = values.encode() + b"\0"
            count = len(raw)
        elif typ == SHORT:
            raw = struct.pack(e + f"{len(values)}H", *values)
            count = len(values)
        elif typ == LONG:
            raw = struct.pack(e + f"{len(values)}I", *values)
            count = len(values)
        elif typ == FLOAT:
            raw = struct.pack(e + f"{len(values)}f", *values)
            count = len(values)
        elif typ == SRATIONAL:
            pairs = [srational(v) for v in values]
            raw = b"".join(struct.pack(e + "ii", n, d) for n, d in pairs)
            count = len(values)
        else:
            raise ValueError(typ)
        if len(raw) <= 4:
            value = raw.ljust(4, b"\0")
        else:
            value = struct.pack(e + "I", data_offset + len(payload))
            payload += raw + (b"\0" if len(raw) % 2 else b"")
        directory += struct.pack(e + "HHI", tag, typ, count) + value
    directory += struct.pack(e + "I", 0)
    header = (b"MM" if big_endian else b"II") + struct.pack(e + "HI", 0x4352, ifd_offset)
    Path(path).write_bytes(header + directory + payload)


def base_tags(dng, name, dual=True):
    tags = {
        PROFILE_NAME: (ASCII, name),
        UNIQUE_CAMERA_MODEL: (ASCII, dng["model"]),
        COLOR_MATRIX1: (SRATIONAL, dng["cm1"]),
        ILLUMINANT1: (SHORT, [dng["illuminant1"]]),
        EMBED_POLICY: (LONG, [0]),
    }
    if "fm1" in dng and dual:
        tags[FORWARD_MATRIX1] = (SRATIONAL, dng["fm1"])
    if dual and "cm2" in dng:
        tags[COLOR_MATRIX2] = (SRATIONAL, dng["cm2"])
        tags[ILLUMINANT2] = (SHORT, [dng["illuminant2"]])
        if "fm2" in dng:
            tags[FORWARD_MATRIX2] = (SRATIONAL, dng["fm2"])
    return tags


def hue_table(hues, sats, hue_shift=0.0, sat_scale=1.0):
    return [v for _ in range(hues * sats) for v in (hue_shift, sat_scale, 1.0)]


def main():
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    dng = read_dng(sys.argv[1])
    out = Path(sys.argv[2])
    out.mkdir(parents=True, exist_ok=True)
    hues, sats = 36, 8

    write_dcp(out / "roundtrip.dcp", base_tags(dng, "rawr test roundtrip"))
    write_dcp(out / "roundtrip_be.dcp", base_tags(dng, "rawr test roundtrip BE"), big_endian=True)
    write_dcp(out / "single.dcp", base_tags(dng, "rawr test single", dual=False))

    t = base_tags(dng, "rawr test hue +30")
    t[HUE_SAT_DIMS] = (LONG, [hues, sats, 1])
    t[HUE_SAT_DATA1] = (FLOAT, hue_table(hues, sats, 30.0))
    if COLOR_MATRIX2 in t:
        t[HUE_SAT_DATA2] = (FLOAT, hue_table(hues, sats, 30.0))
    write_dcp(out / "hue30.dcp", t)

    if "cm2" in dng:
        t = base_tags(dng, "rawr test split -40/+40")
        t[HUE_SAT_DIMS] = (LONG, [hues, sats, 1])
        t[HUE_SAT_DATA1] = (FLOAT, hue_table(hues, sats, -40.0))
        t[HUE_SAT_DATA2] = (FLOAT, hue_table(hues, sats, 40.0))
        write_dcp(out / "split.dcp", t)

    t = base_tags(dng, "rawr test gray look")
    t[LOOK_DIMS] = (LONG, [hues, sats, 1])
    t[LOOK_DATA] = (FLOAT, hue_table(hues, sats, 0.0, 0.0))
    write_dcp(out / "gray_look.dcp", t)

    t = base_tags(dng, "rawr test S curve")
    curve = []
    for i in range(33):
        x = i / 32
        curve += [x, 0.5 - 0.5 * math.cos(math.pi * x)]
    t[TONE_CURVE] = (FLOAT, curve)
    write_dcp(out / "tone_s.dcp", t)

    print("illuminants", dng["illuminant1"], dng.get("illuminant2"), "->", out)


if __name__ == "__main__":
    main()
