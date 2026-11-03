#!/system/bin/sh
# Run after pushing the two CPU benchmark binaries into this script's directory.
# No camera/UI, settings changes, root, or application reinstall required.
set -eu
cd "$(dirname "$0")"
mkdir -p results
getprop ro.product.model > results/model.txt
getprop ro.soc.model > results/soc.txt
getprop ro.build.fingerprint > results/build.txt
dumpsys battery > results/battery-before.txt
dumpsys thermalservice > results/thermal-before.txt
cat /proc/meminfo > results/meminfo-before.txt
for size in 12mp 50mp; do
    case "$size" in
        12mp) width=4080; height=3064 ;;
        50mp) width=8160; height=6128 ;;
    esac
    round=0
    # ABBA order exposes temperature/order drift. Each process has 3 warmups +
    # 20 timed iterations. Saving one JPEG happens in an extra untimed iteration.
    for variant in baseline candidate candidate baseline; do
        round=$((round + 1))
        if [ "$variant" = baseline ]; then mode=flat; else mode=segments; fi
        stem="results/$size-$round-$variant"
        dumpsys thermalservice > "$stem-thermal.txt"
        "./jpeg_bench_${variant}_android" "$mode" "$width" "$height" 98 1 20 \
            "results/$size-$variant.jpg" > "$stem.json" 2> "$stem-samples.jsonl"
        cat "$stem.json"
    done
    cmp "results/$size-baseline.jpg" "results/$size-candidate.jpg"
done
dumpsys thermalservice > results/thermal-after.txt
dumpsys battery > results/battery-after.txt
echo "PASS: baseline/candidate JPEG bytes identical at both resolutions"
