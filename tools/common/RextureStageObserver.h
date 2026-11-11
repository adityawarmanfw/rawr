#pragma once
// The Rexture stage observer shared by the desktop replay tool and the on-device check: after each stage group it dumps the
// arena images the NumPy comparison needs (tmp/indep/m7/compare.py layout) and optionally replaces the estimated fine flow
// by a reference dump's flow. The stages up to the robustness maps match the NumPy spec; the per-frame weight
// normalisation (accumulators, finalize) and the grain pass are Rexture additions the spec does not have ("final" is the
// merge output after both).
#include <rawr/raw_gpu_pipeline/AndroidBurstCoordinator.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <string>
#include <vector>

#include "StageDump.h"

namespace stagedump {

inline std::function<void(const char*, std::uint32_t, const rawr::raw_gpu_pipeline::ResourceArena&)> makeRextureObserver(
    StageDump& dump, std::string injectFlowDir = {}) {
    return [&dump, injectFlowDir](const char* stage, std::uint32_t frame, const rawr::raw_gpu_pipeline::ResourceArena& arena) {

            const std::string st = stage;
            const auto dumpPrepare = [&](const std::string& who, const std::string& prefix) {
                for (int l = 0; l < 4; ++l)
                    dump.image(prefix + "guide_l" + std::to_string(l), arena.image(who + "l" + std::to_string(l)), 1);
                dump.image(prefix + "mean", arena.image(who + "mean"), 3);
                dump.image(prefix + "var", arena.image(who + "var"), 3);
            };
            if (st == "base_prepare") {
                dumpPrepare("rx_base_", "base/");
                dump.image("base/tensor_raw", arena.image("rx_base_tensor"), 3);
            } else if (st == "base_kernel") {
                dump.image("base/tensor_int", arena.image("rx_alt_var"), 3);
                dump.image("base/cov", arena.image("rx_base_cov"), 3);
                dump.image("base/qinv", arena.image("rx_dbg_qinv"), 3);
            } else if (st == "companion_align") {
                const std::string pre = "frames/" + std::to_string(frame) + "/";
                for (int l = 0; l < 4; ++l) {
                    const std::string ls = std::to_string(l);
                    dump.image(pre + "L" + ls + "_lk", arena.image("rx_flow_l" + ls), 2);
                    dump.image(pre + "L" + ls + "_bm", arena.image("rx_dbg_bm_l" + ls), 2);
                    dump.image(pre + "L" + ls + "_prior", arena.image("rx_dbg_prior_l" + ls), 2);
                    dump.image(pre + "L" + ls + "_cost", arena.image("rx_bmcost_l" + ls), 1);
                    dump.image(pre + "L" + ls + "_tensor", arena.image("rx_tiles_l" + ls), 3);
                    dump.image(pre + "L" + ls + "_mode", arena.image("rx_tiles_l" + ls), 1, 3);
                }
                dump.image(pre + "flow", arena.image("rx_flow_l3"), 2);
                if (!injectFlowDir.empty()) {   // verification: continue with the reference's flow
                    const auto file = std::filesystem::path(injectFlowDir) / ("frames__" + std::to_string(frame) + "__flow.bin");
                    std::ifstream in(file, std::ios::binary | std::ios::ate);
                    if (!in) throw std::runtime_error("cannot open " + file.string());
                    const auto bytes = static_cast<std::size_t>(in.tellg());
                    in.seekg(0);
                    std::vector<float> flow(bytes / sizeof(float));
                    in.read(reinterpret_cast<char*>(flow.data()), static_cast<std::streamsize>(bytes));
                    dump.upload(arena.image("rx_flow_l3"), flow);
                }
            } else if (st == "companion_robust") {
                const std::string pre = "frames/" + std::to_string(frame) + "/";
                dump.image(pre + "pre", arena.image("rx_dbg_pre"), 1);
                dump.image(pre + "er", arena.image("rx_er"), 1);
                dump.image(pre + "scale", arena.image("rx_dbg_scale"), 1);
            } else if (st == "final") {
                // linear_output is the merge result (after the grain pass); the debug images hold the finalize output.
                dump.image("final", arena.image("linear_output"), 3);
                dump.image("neff", arena.image("rx_dbg_fin_out"), 1, 3);
                dump.image("pre_grain", arena.image("rx_dbg_fin_pre"), 3);
                dump.image("conf", arena.image("rx_dbg_fin_pre"), 1, 3);
                dump.image("noise_m", arena.image("rx_lc_m"), 1);   // output noise variance relative to one frame, per 4 x 4 cell
            } else if (st == "companions_done") {
                dump.image("support", arena.image("rx_support"), 1);
                dump.image("acc/s", arena.image("rx_acc_a"), 3);
                dump.gather("acc/w", {{&arena.image("rx_acc_a"), 3}, {&arena.image("rx_acc_b"), 0}, {&arena.image("rx_acc_b"), 1}});
                dump.image("acc/sq", arena.image("rx_acc_c"), 3);
                dump.image("acc/ss2", arena.image("rx_dbg_ss2"), 3);
            } else if (st == "base_demosaic") {
                dump.image("base/rcd", arena.image("rx_base_rcd"), 3);
            } else if (st == "companion_prepare") {
                dumpPrepare("rx_alt_", "frames/" + std::to_string(frame) + "/");
            }
    };
}

}  // namespace stagedump
