#!/usr/bin/env sh
# Regenerates shaders/spv/<pass>.spv from shaders/amaze.wgsl with naga, then
# validates each module for Vulkan 1.1. Requires `naga` (cargo install naga-cli)
# and spirv-dis/spirv-as/spirv-val on PATH, or NAGA / SPIRV_* set explicitly.
set -eu
ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd)
NAGA=${NAGA:-naga}
SPIRV_VAL=${SPIRV_VAL:-spirv-val}
SPIRV_DIS=${SPIRV_DIS:-spirv-dis}
SPIRV_AS=${SPIRV_AS:-spirv-as}
SRC="$ROOT/shaders/amaze.wgsl"
OUT="$ROOT/shaders/spv"
mkdir -p "$OUT"
# Order matters: src/Pipeline.cpp records the passes in this sequence.
for PASS in amaze_gradients amaze_candidates amaze_choose amaze_cddiff amaze_weights \
            amaze_nyquist amaze_nyquist_vote amaze_area amaze_hvwt_seq amaze_green \
            amaze_refine amaze_diagonal amaze_pmwt_seq amaze_rbint amaze_correct \
            amaze_chroma amaze_finish; do
  "$NAGA" --entry-point "$PASS" --shader-stage compute "$SRC" "$OUT/$PASS.spv"
  # naga decorates every array with ArrayStride; Vulkan forbids explicit layout
  # on Workgroup storage. Drop it from fixed-size arrays no struct references
  # (here: only the var<workgroup> rows).
  TMP="$OUT/$PASS.spvasm"
  "$SPIRV_DIS" --raw-id "$OUT/$PASS.spv" -o "$TMP"
  awk 'NR == FNR {
         if ($2 == "=" && $3 == "OpTypeArray") arr[$1] = 1
         if ($2 == "=" && $3 == "OpTypeStruct") for (i = 4; i <= NF; i++) used[$i] = 1
         next
       }
       !($1 == "OpDecorate" && $3 == "ArrayStride" && ($2 in arr) && !($2 in used))' "$TMP" "$TMP" > "$TMP.new"
  "$SPIRV_AS" --target-env vulkan1.1 "$TMP.new" -o "$OUT/$PASS.spv"
  rm -f "$TMP" "$TMP.new"
  "$SPIRV_VAL" --target-env vulkan1.1 "$OUT/$PASS.spv"
done
echo "AMaZE SPIR-V regenerated: $("$NAGA" --version)"
