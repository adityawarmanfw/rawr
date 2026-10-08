#include "rcd_balance.h"
#include "rcd_balance_reduce.h"
#include "raw_demosaic/EmbeddedShaders.hpp"

#include <cstring>
#include <stdexcept>
#include <string>
#include <utility>
#include <vector>

#include "dual_auto_refine.h"
#include "dual_auto_select.h"
#include "dual_auto_threshold.h"
#include "dual_auto_tiles.h"
#include "dual_blend.h"
#include "dual_gauss_h.h"
#include "dual_gauss_v.h"
#include "dual_luminance.h"
#include "dual_mask.h"
#include "quadfix_blend.h"
#include "quadfix_demod.h"
#include "quadfix_energy.h"
#include "quadfix_gauss_h.h"
#include "quadfix_gauss_v.h"
#include "quadfix_guide.h"
#include "quadfix_guide_fast.h"
#include "quadfix_mask_h.h"
#include "quadfix_mask_v.h"
#include "rcd_diagonal.h"
#include "rcd_direction.h"
#include "rcd_export.h"
#include "rcd_green.h"
#include "rcd_green_sites.h"
#include "vng4_export.h"
#include "vng4_export_blend.h"
#include "vng4_green.h"
#include "vng4_linear.h"

namespace raw_demosaic {
namespace {
std::vector<uint32_t> words(const unsigned char* bytes, size_t size) {
    if ((size % sizeof(uint32_t)) != 0) throw std::runtime_error("embedded demosaic SPIR-V is not word aligned");
    std::vector<uint32_t> out(size / sizeof(uint32_t));
    std::memcpy(out.data(), bytes, size);
    return out;
}
[[noreturn]] void unknown(const char* family, std::string_view name) {
    throw std::runtime_error(std::string("unknown embedded ") + family + " shader: " + std::string(name));
}
}  // namespace

::rcd::ShaderProvider embeddedRcdShaders() {
    return [](std::string_view name) -> std::vector<uint32_t> {
        if (name == "rcd_direction.comp") return words(rcd_direction_spv, rcd_direction_spv_size);
        if (name == "rcd_green.comp") return words(rcd_green_spv, rcd_green_spv_size);
        if (name == "rcd_diagonal.comp") return words(rcd_diagonal_spv, rcd_diagonal_spv_size);
        if (name == "rcd_green_sites.comp") return words(rcd_green_sites_spv, rcd_green_sites_spv_size);
        if (name == "rcd_export.comp") return words(rcd_export_spv, rcd_export_spv_size);
        if(name=="rcd_balance.comp") return words(rcd_balance_spv,rcd_balance_spv_size);
        if(name=="rcd_balance_reduce.comp") return words(rcd_balance_reduce_spv,rcd_balance_reduce_spv_size);
        unknown("RCD", name);
    };
}

::vng4::ShaderProvider embeddedVng4Shaders() {
    return [](std::string_view name) -> std::vector<uint32_t> {
        if (name == "vng4_linear.comp") return words(vng4_linear_spv, vng4_linear_spv_size);
        if (name == "vng4_green.comp") return words(vng4_green_spv, vng4_green_spv_size);
        if (name == "vng4_export.comp") return words(vng4_export_spv, vng4_export_spv_size);
        if (name == "vng4_export_blend.comp") return words(vng4_export_blend_spv, vng4_export_blend_spv_size);
        unknown("VNG4", name);
    };
}

::dual::ShaderProvider embeddedDualShaders() {
    auto vng = embeddedVng4Shaders();
    auto rcd = embeddedRcdShaders();
    return [vng = std::move(vng), rcd = std::move(rcd)](std::string_view name) -> std::vector<uint32_t> {
        if (name == "dual_luminance.comp") return words(dual_luminance_spv, dual_luminance_spv_size);
        if (name == "dual_mask.comp") return words(dual_mask_spv, dual_mask_spv_size);
        if (name == "dual_gauss_h.comp") return words(dual_gauss_h_spv, dual_gauss_h_spv_size);
        if (name == "dual_gauss_v.comp") return words(dual_gauss_v_spv, dual_gauss_v_spv_size);
        if (name == "dual_blend.comp") return words(dual_blend_spv, dual_blend_spv_size);
        if (name == "dual_auto_tiles.comp") return words(dual_auto_tiles_spv, dual_auto_tiles_spv_size);
        if (name == "dual_auto_select.comp") return words(dual_auto_select_spv, dual_auto_select_spv_size);
        if (name == "dual_auto_refine.comp") return words(dual_auto_refine_spv, dual_auto_refine_spv_size);
        if (name == "dual_auto_threshold.comp") return words(dual_auto_threshold_spv, dual_auto_threshold_spv_size);
        // The blend reuses the frozen RCD passes and a VNG4 sub-pipeline.
        if (name.rfind("vng4_", 0) == 0) return vng(name);
        if (name.rfind("rcd_", 0) == 0) return rcd(name);
        unknown("dual", name);
    };
}

::quadfix::ShaderProvider embeddedQuadfixShaders() {
    return [](std::string_view name) -> std::vector<uint32_t> {
        if (name == "quadfix_guide.comp") return words(quadfix_guide_spv, quadfix_guide_spv_size);
        if (name == "quadfix_guide_fast.comp") return words(quadfix_guide_fast_spv, quadfix_guide_fast_spv_size);
        if (name == "quadfix_energy.comp") return words(quadfix_energy_spv, quadfix_energy_spv_size);
        if (name == "quadfix_demod.comp") return words(quadfix_demod_spv, quadfix_demod_spv_size);
        if (name == "quadfix_gauss_h.comp") return words(quadfix_gauss_h_spv, quadfix_gauss_h_spv_size);
        if (name == "quadfix_gauss_v.comp") return words(quadfix_gauss_v_spv, quadfix_gauss_v_spv_size);
        if (name == "quadfix_mask_h.comp") return words(quadfix_mask_h_spv, quadfix_mask_h_spv_size);
        if (name == "quadfix_mask_v.comp") return words(quadfix_mask_v_spv, quadfix_mask_v_spv_size);
        if (name == "quadfix_blend.comp") return words(quadfix_blend_spv, quadfix_blend_spv_size);
        unknown("quadfix", name);
    };
}
}  // namespace raw_demosaic
