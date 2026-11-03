#!/system/bin/sh
# Push the standalone native/raw_preview build (executables + SPIR-V) first.
# Measures compute GPU timestamps, not Camera2 or displayed VF latency/FPS.
set -eu
cd "$(dirname "$0")/raw-preview-device"
mkdir -p results
./raw_preview_cfa_state_validate > results/cfa-validation.txt 2>&1
./raw_preview_validate > results/rgb-validation.txt 2>&1
for method in 0 1; do
    for scene in normal stress; do
        dumpsys thermalservice > "results/method-$method-$scene-thermal.txt"
        if [ "$scene" = normal ]; then selector=--normal-only; else selector=--stress-only; fi
        ./raw_preview_cfa_state_benchmark --width 4080 --height 3064 \
            --method "$method" --frames 1800 --fps 30 "$selector" \
            > "results/method-$method-$scene.txt" 2>&1
        cat "results/method-$method-$scene.txt"
    done
done
echo "DONE: validation passed; review benchmark timing thresholds in result files"
