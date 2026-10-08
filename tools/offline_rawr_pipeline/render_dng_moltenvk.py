#!/usr/bin/env python3
"""Replay a DNG with production DngSource and the host Vulkan still stages.

Run with `uv run --with numpy --with pillow` (or `uv run --no-project --python
...` when those packages are installed). The host renderer writes SDR PPM,
HDR RGBA16F, RGB gain map, and JPEG_R using the production UltraHDR mux.
"""

import argparse
import hashlib
import json
import subprocess
import sys
from pathlib import Path

import numpy as np
from PIL import Image


ROOT = Path(__file__).resolve().parents[2]


def run(argv):
    print("+", " ".join(map(str, argv)), flush=True)
    subprocess.run(list(map(str, argv)), check=True)


def info(path):
    return dict(line.strip().split("=", 1) for line in path.read_text().splitlines())


def pack_cfa(cfa_path, original_path, shading_path, width, height, out, clip, apply_shading):
    shape = (height, width)
    cfa = np.memmap(cfa_path, dtype="<f4", mode="r", shape=shape)
    original = np.memmap(original_path, dtype="<f4", mode="r", shape=shape)
    shading = np.memmap(shading_path, dtype="<f4", mode="r", shape=shape) if apply_shading else None
    # The production packed-image path stores CFA values in R,G1,G2,B order.
    packed = np.empty((height//2, width//2, 4), dtype="<f2")
    for i, (y, x) in enumerate(((0, 0), (0, 1), (1, 0), (1, 1))):
        values = cfa[y::2, x::2] * shading[y::2, x::2] if shading is not None else cfa[y::2, x::2]
        if not np.isfinite(values).all() or np.max(np.abs(values)) > 16:
            raise ValueError("CFA override contains invalid values")
        packed[..., i] = np.clip(values, 0, 16)
        packed[..., i].view("<u2")[original[y::2, x::2] >= clip] |= np.uint16(0x8000)
    packed.tofile(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("dng", type=Path)
    parser.add_argument("output", type=Path)
    method = parser.add_mutually_exclusive_group()
    method.add_argument("--cfa", type=Path, help="pre-shading float32 CFA override")
    method.add_argument("--app-coloropp", action="store_true", help="run the app's embedded Coloropp stages directly")
    parser.add_argument("--recovery", choices=("on", "off", "preserve"),
                        help="post-demosaic policy: on=current, off=common-ceiling clip, preserve=keep pre-demosaic result")
    parser.add_argument("--clip", type=float, default=0.987)
    parser.add_argument("--hlth", type=float, default=1.0, help="Coloropp threshold gain (RawTherapee Hlth)")
    parser.add_argument("--app-compression", type=float, default=100.0,
                        help="app Coloropp SDR highlight compression, 0..300")
    parser.add_argument("--rt-compression", type=float, default=0.0,
                        help="offline RawTherapee-style highlight compression control, 0..500")
    parser.add_argument("--highlight-bias", type=float, default=0.0,
                        help="production tonemap Highlights control for comparison")
    parser.add_argument("--post-gain", type=float,
                        help="override the recipe's AE post gain for an exposure diagnostic")
    parser.add_argument("--build-dir", type=Path, default=ROOT/"tmp/offline_rawr_pipeline")
    parser.add_argument("--skip-build", action="store_true")
    args = parser.parse_args()
    if not args.dng.is_file():
        parser.error("DNG file is missing")
    if not -100.0 <= args.highlight_bias <= 100.0:
        parser.error("Highlights control must be between -100 and 100")
    if not 0 <= args.rt_compression <= 500:
        parser.error("RT highlight compression must be 0..500")
    if not 0 <= args.app_compression <= 300:
        parser.error("App highlight compression must be 0..300")
    if args.post_gain is not None and not 0.0 < args.post_gain <= 16.0:
        parser.error("AE post gain must be in (0, 16]")
    args.output.mkdir(parents=True, exist_ok=True)
    if not args.skip_build:
        run(["cmake", "-S", ROOT/"tools/offline_rawr_pipeline", "-B", args.build_dir,
             "-G", "Ninja", "-DCMAKE_BUILD_TYPE=Release"])
        targets = ["rawr_dng_decode", "rawr_offline_pipeline", "rawr_uhdr_mux"]
        run(["cmake", "--build", args.build_dir, "--target", *targets])
    digest = hashlib.sha256(args.dng.read_bytes()).hexdigest()
    decoded = args.output/"decoded"
    if not (decoded/"input.sha256").exists() or (decoded/"input.sha256").read_text().strip() != digest:
        run([args.build_dir/"rawr_dng_decode", args.dng, decoded])
        (decoded/"input.sha256").write_text(digest+"\n")
    details = info(decoded/"dng_info.txt")
    width, height = int(details["width"]), int(details["height"])
    if width % 2 or height % 2:
        raise ValueError("Production packed CFA replay requires even dimensions")
    if int(details["cfa"]) != 0:
        raise ValueError("Host packed CFA replay currently supports RGGB only")
    try:
        provenance = json.loads((decoded/"provenance.json").read_text())
        extension = provenance["extensions"]
        recipe = json.loads(extension["com.rawrcam.processing.recipe.v1"])
        resolved = json.loads(extension["com.rawrcam.processing.resolved.v1"])
        frame = json.loads(extension["com.rawrcam.processing.frame.v1"])
    except (ValueError, KeyError) as error:
        raise ValueError("Host replay needs a RAWR DNG with embedded recipe and resolved frame") from error
    if recipe["demosaicAlgorithm"] != "DualRcdVng4":
        raise ValueError("Host replay currently supports DualRcdVng4 DNGs")
    if recipe.get("filmSimEnabled"):
        raise ValueError("Host replay currently supports the RawrBase tonemap")
    tone = recipe["tone"]
    unsupported_tone = ("blacks", "shadows", "contrast", "midtones", "highlights", "whites",
                        "saturation", "vibrance", "wbTemperature", "wbTint")
    if any(abs(float(tone.get(key, 0))) > 1e-6 for key in unsupported_tone) or float(tone.get("colorRenderingStrength", 1)) != 1:
        raise ValueError("Host replay currently supports neutral tone controls plus render exposure")
    if tone.get("outputColorSpaceId", "output.srgb") != "output.srgb":
        raise ValueError("Host replay currently supports sRGB output")
    subsampling = {"jpeg.444": 0, "jpeg.422": 1, "jpeg.420": 2}.get(tone.get("jpegChromaSubsamplingId", "jpeg.444"))
    if subsampling is None:
        raise ValueError("Unsupported JPEG chroma subsampling")
    recovery_mode = args.recovery or ("on" if args.app_coloropp else
                                     "preserve" if args.cfa else
                                     ("on" if recipe.get("highlightReconstructionEnabled", True) else "off"))
    recovery_on = recovery_mode == "on"
    if args.app_coloropp and (not recovery_on or not 0.5 <= args.hlth <= 2.0 or args.rt_compression != 0):
        raise ValueError("App Coloropp requires recovery on, Hlth 0.5..2, and no separate RT compression")
    want_uhdr = bool(recipe.get("ultraHdrEnabled", False))
    post_gain = args.post_gain if args.post_gain is not None else resolved["aePostGain"]
    source = args.cfa if args.cfa else decoded/"input_cfa.f32"
    if not source.is_file() or source.stat().st_size != width*height*4:
        raise ValueError("CFA source size is wrong")
    packed = args.output/"input.rgba16f"
    pack_cfa(source, decoded/"input_cfa.f32", decoded/"shading_multiplier.f32",
             width, height, packed, args.clip, bool(recipe.get("lensShadingCorrectionEnabled", True)))
    camera = np.asarray(frame["cameraToLinearSrgbRowMajor"], np.float64).reshape(3, 3)
    ap1 = np.asarray(((.613097, .339523, .047380), (.070194, .916354, .013452),
                      (.020616, .109570, .869815)))
    working = (ap1 @ camera).T.flatten()
    wb4 = frame["whiteBalanceRggb"]
    wb = (wb4[0], (wb4[1]+wb4[2])/2, wb4[3])
    config = {
        "source": args.dng.stem,
        "width": width, "height": height, "cfa": details["cfa"],
        "fccSteps": recipe["fccSteps"],
        "fccEdgeSigma": .08,
        "fccChromaBound": 1.0,
        "defringeStrength": (recipe.get("defringeStrength", 1.0) if recipe.get("defringeEnabled", True) else 0.0),
        "defringeEdgeThreshold": recipe.get("defringeEdgeThreshold", .02),
        "defringeLumaFloor": recipe.get("defringeLumaFloor", .08),
        "dualAutoContrast": int(recipe["dualAutoContrast"]),
        "dualContrastPercent": recipe["dualContrastPercent"],
        "exposureEV": recipe["tone"]["renderExposure"],
        "wbRgb": ",".join(map(str, wb)),
        "cameraToWorkingColumnMajor": ",".join(map(str, working)),
    }
    (args.output/"input.rgba16f.txt").write_text("".join(f"{k}={v}\n" for k, v in config.items()))
    rendered = args.output/"rendered"
    command = [args.build_dir/"rawr_offline_pipeline", "--input", packed,
               "--config", args.output/"input.rgba16f.txt", "--output-dir", rendered,
               "--single-recovery", "1" if recovery_on else "0",
               "--post-gain", post_gain, "--highlight-bias", args.highlight_bias,
               "--rt-highlight-compression", args.rt_compression]
    if recovery_mode == "preserve":
        command.append("--preserve-reconstructed")
    if args.app_coloropp:
        command += ["--app-coloropp", "--app-hlth", args.hlth,
                    "--app-compression", args.app_compression]
    if want_uhdr:
        command += ["--gainmap", "--gainmap-rgb", "--dump-hdr", "--gainmap-matrix",
                    ",".join(map(str, camera.flatten())), "--gainmap-exposure",
                    post_gain * 2**recipe["tone"]["renderExposure"]]
    quadfix_applied = bool(recipe.get("quadfixEnabled") and width % 16 == 0 and height % 16 == 0)
    if quadfix_applied:
        command += ["--quadfix-fast-median" if recipe.get("quadfixFastMedian") else "--quadfix"]
    run(command)
    stem = "recon_on_h0" if recovery_on else "recon_off_h0"
    base_jpeg, map_jpeg, final_jpeg = (rendered/"base.jpg", rendered/"gainmap.jpg", rendered/"render.jpg")
    Image.open(rendered/f"{stem}.ppm").convert("RGB").save(
        base_jpeg, format="JPEG", quality=int(tone.get("jpegQuality", 98)), subsampling=subsampling)
    if want_uhdr:
        map_info = json.loads((rendered/f"{stem}.gainmap.json").read_text())
        rgba = np.fromfile(rendered/f"{stem}.gainmap.rgba8", np.uint8).reshape(
            map_info["height"], map_info["width"], 4)
        Image.fromarray(rgba[:, :, :3].copy(), "RGB").save(
            map_jpeg, format="JPEG", quality=95, subsampling=0)
        mux = [args.build_dir/"rawr_uhdr_mux", "--base", base_jpeg, "--map", map_jpeg,
               "--out", final_jpeg, "--min-log2", map_info["minLog2"],
               "--max-log2", map_info["maxLog2"], "--gamma", map_info["gamma"],
               "--off-sdr", map_info["offsetSdr"], "--off-hdr", map_info["offsetHdr"],
               "--cap-min", map_info["hdrCapacityMin"], "--cap-max", map_info["hdrCapacityMax"]]
        if map_info["multiChannel"]:
            mux.append("--multi")
        run(mux)
    else:
        final_jpeg.write_bytes(base_jpeg.read_bytes())
    data = final_jpeg.read_bytes()
    if not data.startswith(b"\xff\xd8") or (want_uhdr and (b"MPF\0" not in data or b"hdr-gain-map" not in data)):
        raise RuntimeError("MoltenVK JPEG container validation failed")
    manifest = {"dng_sha256": digest, "cfa_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
                "recipe": recipe, "resolved": resolved, "frame": frame,
                "gpu": "MoltenVK",
                "app_coloropp": args.app_coloropp,
                "coloropp_hlth": args.hlth if args.app_coloropp else None,
                "ultraHdr": want_uhdr, "postRgbRecovery": recovery_on,
                "postRgbMode": recovery_mode,
                "tonemapHighlightBias": args.highlight_bias,
                "rtHighlightCompression": args.rt_compression,
                "tonemapPostGain": post_gain,
                "quadfix_applied": quadfix_applied,
                "quadfix_skipped_reason": ("Frame dimensions are not multiples of 16; app RendererEngine also skips its Dual quadfix prepass"
                                           if recipe.get("quadfixEnabled") and not quadfix_applied else None),
                "stage_limitations": [
                    "Host path uses the production Dual, post-demosaic, tonemap and gainmap components",
                    "When dimensions permit, host quadfix runs full-frame while the app uses tiles with halo",
                    "Host uses Pillow for JPEG encoding and the production UltraHDR mux",
                    "App-only geometry and metadata stages are exercised by the Android debug replayer",
                ]}
    (args.output/"replay_manifest.json").write_text(json.dumps(manifest, indent=2)+"\n")
    print("Host DNG replay complete:", rendered)


if __name__ == "__main__":
    main()
