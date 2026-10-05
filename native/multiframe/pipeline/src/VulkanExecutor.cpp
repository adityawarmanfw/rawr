#include <rawr/raw_gpu_pipeline/VulkanExecutor.h>

#include <algorithm>
#include <array>
#include <stdexcept>
#include <string>

#include "mf_a11_finalize_prod.h"
#include "mf_a11_finalize_trace.h"
#include "mf_accumulate_prod.h"
#include "mf_accumulate_trace.h"
#include "mf_affine_fit_prod.h"
#include "mf_affine_fit_trace.h"
#include "mf_affine_gate_prod.h"
#include "mf_noise_estimate_prod.h"
#include "mf_noise_estimate_trace.h"
#include "mf_fallback_chroma_prod.h"
#include "mf_fallback_chroma_trace.h"
#include "mf_hot_pixel_conceal_prod.h"
#include "mf_hot_pixel_conceal_trace.h"
#include "mf_hdrp_prepare_prod.h"
#include "mf_hdrp_prepare_trace.h"
#include "mf_hdrp_hot_pixel_prod.h"
#include "mf_hdrp_hot_pixel_trace.h"
#include "mf_hdrp_avg_pool_prod.h"
#include "mf_hdrp_avg_pool_trace.h"
#include "mf_hdrp_blur_prod.h"
#include "mf_hdrp_blur_trace.h"
#include "mf_hdrp_upsample_align_prod.h"
#include "mf_hdrp_upsample_align_trace.h"
#include "mf_hdrp_correct_upsampling_prod.h"
#include "mf_hdrp_correct_upsampling_trace.h"
#include "mf_hdrp_tile_diff_prod.h"
#include "mf_hdrp_tile_diff_trace.h"
#include "mf_hdrp_best_tile_prod.h"
#include "mf_hdrp_best_tile_trace.h"
#include "mf_hdrp_warp_prod.h"
#include "mf_hdrp_warp_trace.h"
#include "mf_hdrp_color_diff_prod.h"
#include "mf_hdrp_color_diff_trace.h"
#include "mf_hdrp_column_sum_prod.h"
#include "mf_hdrp_column_sum_trace.h"
#include "mf_hdrp_mean_prod.h"
#include "mf_hdrp_mean_trace.h"
#include "mf_hdrp_merge_weight_prod.h"
#include "mf_hdrp_merge_weight_trace.h"
#include "mf_hdrp_accumulate_prod.h"
#include "mf_hdrp_accumulate_trace.h"
#include "mf_hdrp_finalize_prod.h"
#include "mf_hdrp_finalize_trace.h"
#include "mf_affine_gate_trace.h"
#include "mf_alignment_local5_prod.h"
#include "mf_alignment_local5_trace.h"
#include "mf_block_match_prod.h"
#include "mf_block_match_trace.h"
#include "mf_flow_dense_smooth_prod.h"
#include "mf_flow_dense_smooth_trace.h"
#include "mf_flow_upsample_prod.h"
#include "mf_flow_upsample_trace.h"
#include "mf_kernel_estimate_prod.h"
#include "mf_kernel_estimate_trace.h"
#include "mf_lk_refine_prod.h"
#include "mf_lk_refine_trace.h"
#include "mf_pyramid_decimate_prod.h"
#include "mf_pyramid_decimate_trace.h"
#include "mf_pyramid_gaussian_prod.h"
#include "mf_pyramid_gaussian_trace.h"
#include "mf_raw_normalize_prod.h"
#include "mf_raw_normalize_trace.h"
#include "mf_reference_accumulate_prod.h"
#include "mf_reference_accumulate_trace.h"
#include "mf_robust_erode5_prod.h"
#include "mf_robust_erode5_trace.h"
#include "mf_robust_flow_scale_prod.h"
#include "mf_robust_flow_scale_trace.h"
#include "mf_robust_guide_prod.h"
#include "mf_robust_guide_trace.h"
#include "mf_robust_photometric_prod.h"
#include "mf_robust_photometric_trace.h"
#include "mf_robust_stats_prod.h"
#include "mf_robust_stats_trace.h"
#include "mf_robust_warp_prod.h"
#include "mf_robust_warp_trace.h"
#include "mf_support_accumulate_prod.h"
#include "mf_support_accumulate_trace.h"
namespace rawr::raw_gpu_pipeline {
namespace {
struct B {
    uint32_t n;
    VkDescriptorType t;
};
struct Spec {
    const char* name;
    const unsigned char* prod;
    size_t prodN;
    const unsigned char* trace;
    size_t traceN;
    std::vector<B> b;
};
const std::array<Spec, static_cast<size_t>(ShaderId::Count)> specs{{{"raw_normalize",
                                   mf_raw_normalize_prod_spv,
                                   mf_raw_normalize_prod_spv_size,
                                   mf_raw_normalize_trace_spv,
                                   mf_raw_normalize_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"alignment_local5",
                                   mf_alignment_local5_prod_spv,
                                   mf_alignment_local5_prod_spv_size,
                                   mf_alignment_local5_trace_spv,
                                   mf_alignment_local5_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"pyramid_gaussian",
                                   mf_pyramid_gaussian_prod_spv,
                                   mf_pyramid_gaussian_prod_spv_size,
                                   mf_pyramid_gaussian_trace_spv,
                                   mf_pyramid_gaussian_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"pyramid_decimate",
                                   mf_pyramid_decimate_prod_spv,
                                   mf_pyramid_decimate_prod_spv_size,
                                   mf_pyramid_decimate_trace_spv,
                                   mf_pyramid_decimate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"flow_upsample",
                                   mf_flow_upsample_prod_spv,
                                   mf_flow_upsample_prod_spv_size,
                                   mf_flow_upsample_trace_spv,
                                   mf_flow_upsample_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"block_match",
                                   mf_block_match_prod_spv,
                                   mf_block_match_prod_spv_size,
                                   mf_block_match_trace_spv,
                                   mf_block_match_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"lk_refine",
                                   mf_lk_refine_prod_spv,
                                   mf_lk_refine_prod_spv_size,
                                   mf_lk_refine_trace_spv,
                                   mf_lk_refine_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"flow_dense_smooth",
                                   mf_flow_dense_smooth_prod_spv,
                                   mf_flow_dense_smooth_prod_spv_size,
                                   mf_flow_dense_smooth_trace_spv,
                                   mf_flow_dense_smooth_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"kernel_estimate",
                                   mf_kernel_estimate_prod_spv,
                                   mf_kernel_estimate_prod_spv_size,
                                   mf_kernel_estimate_trace_spv,
                                   mf_kernel_estimate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"robust_guide",
                                   mf_robust_guide_prod_spv,
                                   mf_robust_guide_prod_spv_size,
                                   mf_robust_guide_trace_spv,
                                   mf_robust_guide_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"robust_stats",
                                   mf_robust_stats_prod_spv,
                                   mf_robust_stats_prod_spv_size,
                                   mf_robust_stats_trace_spv,
                                   mf_robust_stats_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"robust_warp",
                                   mf_robust_warp_prod_spv,
                                   mf_robust_warp_prod_spv_size,
                                   mf_robust_warp_trace_spv,
                                   mf_robust_warp_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"robust_flow_scale",
                                   mf_robust_flow_scale_prod_spv,
                                   mf_robust_flow_scale_prod_spv_size,
                                   mf_robust_flow_scale_trace_spv,
                                   mf_robust_flow_scale_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"robust_photometric",
                                   mf_robust_photometric_prod_spv,
                                   mf_robust_photometric_prod_spv_size,
                                   mf_robust_photometric_trace_spv,
                                   mf_robust_photometric_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                                    {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                                    {8, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"robust_erode5",
                                   mf_robust_erode5_prod_spv,
                                   mf_robust_erode5_prod_spv_size,
                                   mf_robust_erode5_trace_spv,
                                   mf_robust_erode5_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"affine_fit",
                                   mf_affine_fit_prod_spv,
                                   mf_affine_fit_prod_spv_size,
                                   mf_affine_fit_trace_spv,
                                   mf_affine_fit_trace_spv_size,
                                  {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                                    {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                                    {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"affine_gate",
                                   mf_affine_gate_prod_spv,
                                   mf_affine_gate_prod_spv_size,
                                   mf_affine_gate_trace_spv,
                                   mf_affine_gate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER},
                                    {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"support_accumulate",
                                   mf_support_accumulate_prod_spv,
                                   mf_support_accumulate_prod_spv_size,
                                   mf_support_accumulate_trace_spv,
                                   mf_support_accumulate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"accumulate",
                                   mf_accumulate_prod_spv,
                                   mf_accumulate_prod_spv_size,
                                   mf_accumulate_trace_spv,
                                   mf_accumulate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {7, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"reference_accumulate",
                                   mf_reference_accumulate_prod_spv,
                                   mf_reference_accumulate_prod_spv_size,
                                   mf_reference_accumulate_trace_spv,
                                   mf_reference_accumulate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {4, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"a11_finalize",
                                   mf_a11_finalize_prod_spv,
                                   mf_a11_finalize_prod_spv_size,
                                   mf_a11_finalize_trace_spv,
                                   mf_a11_finalize_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"noise_estimate",
                                   mf_noise_estimate_prod_spv,
                                   mf_noise_estimate_prod_spv_size,
                                   mf_noise_estimate_trace_spv,
                                   mf_noise_estimate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"fallback_chroma",
                                   mf_fallback_chroma_prod_spv,
                                   mf_fallback_chroma_prod_spv_size,
                                   mf_fallback_chroma_trace_spv,
                                   mf_fallback_chroma_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE},
                                    {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"hot_pixel_conceal",
                                   mf_hot_pixel_conceal_prod_spv,
                                   mf_hot_pixel_conceal_prod_spv_size,
                                   mf_hot_pixel_conceal_trace_spv,
                                   mf_hot_pixel_conceal_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_prepare",
                                   mf_hdrp_prepare_prod_spv,
                                   mf_hdrp_prepare_prod_spv_size,
                                   mf_hdrp_prepare_trace_spv,
                                   mf_hdrp_prepare_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"hdrp_hot_pixel",
                                   mf_hdrp_hot_pixel_prod_spv,
                                   mf_hdrp_hot_pixel_prod_spv_size,
                                   mf_hdrp_hot_pixel_trace_spv,
                                   mf_hdrp_hot_pixel_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_avg_pool",
                                   mf_hdrp_avg_pool_prod_spv,
                                   mf_hdrp_avg_pool_prod_spv_size,
                                   mf_hdrp_avg_pool_trace_spv,
                                   mf_hdrp_avg_pool_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"hdrp_blur",
                                   mf_hdrp_blur_prod_spv,
                                   mf_hdrp_blur_prod_spv_size,
                                   mf_hdrp_blur_trace_spv,
                                   mf_hdrp_blur_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"hdrp_upsample_align",
                                   mf_hdrp_upsample_align_prod_spv,
                                   mf_hdrp_upsample_align_prod_spv_size,
                                   mf_hdrp_upsample_align_trace_spv,
                                   mf_hdrp_upsample_align_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_correct_upsampling",
                                   mf_hdrp_correct_upsampling_prod_spv,
                                   mf_hdrp_correct_upsampling_prod_spv_size,
                                   mf_hdrp_correct_upsampling_trace_spv,
                                   mf_hdrp_correct_upsampling_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_tile_diff",
                                   mf_hdrp_tile_diff_prod_spv,
                                   mf_hdrp_tile_diff_prod_spv_size,
                                   mf_hdrp_tile_diff_trace_spv,
                                   mf_hdrp_tile_diff_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_best_tile",
                                   mf_hdrp_best_tile_prod_spv,
                                   mf_hdrp_best_tile_prod_spv_size,
                                   mf_hdrp_best_tile_trace_spv,
                                   mf_hdrp_best_tile_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_warp",
                                   mf_hdrp_warp_prod_spv,
                                   mf_hdrp_warp_prod_spv_size,
                                   mf_hdrp_warp_trace_spv,
                                   mf_hdrp_warp_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_color_diff",
                                   mf_hdrp_color_diff_prod_spv,
                                   mf_hdrp_color_diff_prod_spv_size,
                                   mf_hdrp_color_diff_trace_spv,
                                   mf_hdrp_color_diff_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"hdrp_column_sum",
                                   mf_hdrp_column_sum_prod_spv,
                                   mf_hdrp_column_sum_prod_spv_size,
                                   mf_hdrp_column_sum_trace_spv,
                                   mf_hdrp_column_sum_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_mean",
                                   mf_hdrp_mean_prod_spv,
                                   mf_hdrp_mean_prod_spv_size,
                                   mf_hdrp_mean_trace_spv,
                                   mf_hdrp_mean_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_merge_weight",
                                   mf_hdrp_merge_weight_prod_spv,
                                   mf_hdrp_merge_weight_prod_spv_size,
                                   mf_hdrp_merge_weight_trace_spv,
                                   mf_hdrp_merge_weight_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER}}},
                                  {"hdrp_accumulate",
                                   mf_hdrp_accumulate_prod_spv,
                                   mf_hdrp_accumulate_prod_spv_size,
                                   mf_hdrp_accumulate_trace_spv,
                                   mf_hdrp_accumulate_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {3, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}},
                                  {"hdrp_finalize",
                                   mf_hdrp_finalize_prod_spv,
                                   mf_hdrp_finalize_prod_spv_size,
                                   mf_hdrp_finalize_trace_spv,
                                   mf_hdrp_finalize_trace_spv_size,
                                   {{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}, {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE}}}}};
void ck(VkResult r, const char* w) {
    if (r != VK_SUCCESS) throw std::runtime_error(std::string(w) + " VkResult=" + std::to_string(r));
}
VkShaderModule sm(VkDevice d, const unsigned char* p, size_t n) {
    VkShaderModuleCreateInfo c{};
    c.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    c.codeSize = n;
    c.pCode = reinterpret_cast<const uint32_t*>(p);
    VkShaderModule m = VK_NULL_HANDLE;
    ck(vkCreateShaderModule(d, &c, nullptr, &m), "mf shader module");
    return m;
}
}  // namespace
void VulkanExecutor::initialize(VkDevice d, bool wantTrace) {
    reset();
    if (!d) throw std::invalid_argument("multiframe executor: null device");
    // specs[] is indexed positionally by ShaderId: a misordered entry silently
    // binds the wrong pipeline. Verify the full order on every initialize.
    static constexpr const char* kOrder[] = {
        "raw_normalize",      "alignment_local5",   "pyramid_gaussian",  "pyramid_decimate",  "flow_upsample",
        "block_match",        "lk_refine",          "flow_dense_smooth", "kernel_estimate",   "robust_guide",
        "robust_stats",       "robust_warp",        "robust_flow_scale", "robust_photometric", "robust_erode5",
        "affine_fit",         "affine_gate",        "support_accumulate", "accumulate",       "reference_accumulate",
        "a11_finalize",       "noise_estimate",     "fallback_chroma",    "hot_pixel_conceal",
        "hdrp_prepare", "hdrp_hot_pixel", "hdrp_avg_pool", "hdrp_blur", "hdrp_upsample_align", "hdrp_correct_upsampling", "hdrp_tile_diff", "hdrp_best_tile", "hdrp_warp", "hdrp_color_diff", "hdrp_column_sum", "hdrp_mean", "hdrp_merge_weight", "hdrp_accumulate", "hdrp_finalize",
    };
    static_assert(sizeof(kOrder) / sizeof(kOrder[0]) == static_cast<size_t>(ShaderId::Count),
                  "executor spec order table out of sync with ShaderId");
    if (specs.size() != static_cast<size_t>(ShaderId::Count))
        throw std::logic_error("multiframe executor: spec count out of sync with ShaderId");
    for (size_t i = 0; i < specs.size(); ++i)
        if (std::string(specs[i].name) != kOrder[i])
            throw std::logic_error(std::string("multiframe executor: spec/enum order mismatch at ") +
                                   std::to_string(i));
    device_ = d;
    trace_ = wantTrace;
    for (size_t i = 0; i < specs.size(); ++i) {
        const auto& s = specs[i];
        auto& p = programs_[i];
        std::vector<VkDescriptorSetLayoutBinding> bs;
        for (auto b : s.b) bs.push_back({b.n, b.t, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        if (wantTrace) bs.push_back({31, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr});
        VkDescriptorSetLayoutCreateInfo dc{};
        dc.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dc.bindingCount = uint32_t(bs.size());
        dc.pBindings = bs.data();
        ck(vkCreateDescriptorSetLayout(d, &dc, nullptr, &p.dsl), "mf descriptor layout");
        VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, 128};
        VkPipelineLayoutCreateInfo lc{};
        lc.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        lc.setLayoutCount = 1;
        lc.pSetLayouts = &p.dsl;
        lc.pushConstantRangeCount = 1;
        lc.pPushConstantRanges = &pr;
        ck(vkCreatePipelineLayout(d, &lc, nullptr, &p.layout), "mf pipeline layout");
        auto mk = [&](const unsigned char* bytes, size_t n, VkPipeline* out) {
            auto m = sm(d, bytes, n);
            VkPipelineShaderStageCreateInfo st{};
            st.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            st.stage = VK_SHADER_STAGE_COMPUTE_BIT;
            st.module = m;
            st.pName = "main";
            VkComputePipelineCreateInfo pc{};
            pc.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
            pc.stage = st;
            pc.layout = p.layout;
            auto r = vkCreateComputePipelines(d, VK_NULL_HANDLE, 1, &pc, nullptr, out);
            vkDestroyShaderModule(d, m, nullptr);
            ck(r, "mf compute pipeline");
        };
        mk(s.prod, s.prodN, &p.prod);
        if (wantTrace) mk(s.trace, s.traceN, &p.trace);
        std::vector<VkDescriptorPoolSize> ps;
        uint32_t ni = 0, nb = 0;
        for (auto b : s.b) (b.t == VK_DESCRIPTOR_TYPE_STORAGE_IMAGE ? ni : nb)++;
        if (wantTrace) nb++;
        if (ni) ps.push_back({VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, ni});
        if (nb) ps.push_back({VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nb});
        constexpr uint32_t kSetsPerProgram = 256;
        for (auto& q : ps) q.descriptorCount *= kSetsPerProgram;
        VkDescriptorPoolCreateInfo pci{};
        pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        pci.flags = 0;
        pci.maxSets = kSetsPerProgram;
        pci.poolSizeCount = uint32_t(ps.size());
        pci.pPoolSizes = ps.data();
        ck(vkCreateDescriptorPool(d, &pci, nullptr, &p.pool), "mf descriptor pool");
    }
}
void VulkanExecutor::record(VkCommandBuffer cmd, ShaderId id, const DispatchBindings& in, const void* push,
                            uint32_t pushN, uint32_t gx, uint32_t gy, uint32_t gz, const TraceTarget* tr) {
    if (!device_ || !cmd || !gx || !gy || !gz || pushN > 128 || (pushN && !push))
        throw std::invalid_argument("multiframe executor: invalid dispatch");
    size_t i = static_cast<size_t>(id);
    if (i >= specs.size()) throw std::invalid_argument("multiframe executor: invalid shader");
    const auto& s = specs[i];
    auto& p = programs_[i];
    bool useTrace = tr && tr->buffer;
    if (useTrace && !trace_) throw std::logic_error("multiframe executor: trace pipeline not initialized");
    for (const auto expected : s.b) {
        uint32_t count = 0;
        for (const auto& x : in.images)
            if (x.binding == expected.n) {
                if (expected.t != VK_DESCRIPTOR_TYPE_STORAGE_IMAGE)
                    throw std::invalid_argument("multiframe executor: descriptor type mismatch");
                ++count;
            }
        for (const auto& x : in.buffers)
            if (x.binding == expected.n) {
                if (expected.t != VK_DESCRIPTOR_TYPE_STORAGE_BUFFER)
                    throw std::invalid_argument("multiframe executor: descriptor type mismatch");
                ++count;
            }
        if (count != 1) throw std::invalid_argument("multiframe executor: missing/duplicate required binding");
    }
    for (const auto& x : in.images) {
        bool known = false;
        for (const auto e : s.b) known |= e.n == x.binding;
        if (!known) throw std::invalid_argument("multiframe executor: unexpected image binding");
    }
    for (const auto& x : in.buffers) {
        bool known = false;
        for (const auto e : s.b) known |= e.n == x.binding;
        if (!known) throw std::invalid_argument("multiframe executor: unexpected buffer binding");
    }
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = p.pool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &p.dsl;
    ck(vkAllocateDescriptorSets(device_, &ai, &set), "mf transient descriptor set");
    std::vector<VkDescriptorImageInfo> ii;
    std::vector<VkDescriptorBufferInfo> bi;
    std::vector<VkWriteDescriptorSet> w;
    ii.reserve(in.images.size());
    bi.reserve(in.buffers.size() + 1);
    w.reserve(in.images.size() + in.buffers.size() + 1);
    for (auto x : in.images) {
        ii.push_back({VK_NULL_HANDLE, x.view, x.layout});
        VkWriteDescriptorSet z{};
        z.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        z.dstSet = set;
        z.dstBinding = x.binding;
        z.descriptorCount = 1;
        z.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        z.pImageInfo = &ii.back();
        w.push_back(z);
    }
    for (auto x : in.buffers) {
        bi.push_back({x.buffer, x.offset, x.range});
        VkWriteDescriptorSet z{};
        z.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        z.dstSet = set;
        z.dstBinding = x.binding;
        z.descriptorCount = 1;
        z.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        z.pBufferInfo = &bi.back();
        w.push_back(z);
    }
    if (useTrace) {
        bi.push_back({tr->buffer, 0, tr->range});
        VkWriteDescriptorSet z{};
        z.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        z.dstSet = set;
        z.dstBinding = 31;
        z.descriptorCount = 1;
        z.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        z.pBufferInfo = &bi.back();
        w.push_back(z);
    }
    vkUpdateDescriptorSets(device_, uint32_t(w.size()), w.data(), 0, nullptr);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, useTrace ? p.trace : p.prod);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, p.layout, 0, 1, &set, 0, nullptr);
    if (pushN) vkCmdPushConstants(cmd, p.layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, pushN, push);
    vkCmdDispatch(cmd, gx, gy, gz);
}
void VulkanExecutor::beginBatch() {
    if (!device_) throw std::logic_error("multiframe executor: not initialized");
    for (auto& p : programs_)
        if (p.pool) ck(vkResetDescriptorPool(device_, p.pool, 0), "mf reset descriptor pool");
}
void computeWriteBarrier(VkCommandBuffer c) {
    VkMemoryBarrier b{};
    b.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER;
    b.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
    vkCmdPipelineBarrier(c, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &b, 0,
                         nullptr, 0, nullptr);
}
void VulkanExecutor::reset() noexcept {
    if (device_)
        for (auto& p : programs_) {
            if (p.pool) vkDestroyDescriptorPool(device_, p.pool, nullptr);
            if (p.trace) vkDestroyPipeline(device_, p.trace, nullptr);
            if (p.prod) vkDestroyPipeline(device_, p.prod, nullptr);
            if (p.layout) vkDestroyPipelineLayout(device_, p.layout, nullptr);
            if (p.dsl) vkDestroyDescriptorSetLayout(device_, p.dsl, nullptr);
            p = {};
        }
    device_ = VK_NULL_HANDLE;
    trace_ = false;
}
}  // namespace rawr::raw_gpu_pipeline
