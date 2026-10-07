cmake_minimum_required(VERSION 3.22)
project(raw_gpu_pipeline LANGUAGES CXX)

# Multiframe burst executor. It compiles the alignment (../alignment) and
# Wronski merge (../merge_wronski) shaders it dispatches, in production
# and scalar-trace variants, and embeds them as mf_<name>_{prod,trace}.h.
option(RAW_GPU_PIPELINE_BUILD_TESTS "Build host unit tests" OFF)

if(ANDROID)
  find_library(VULKAN_LIB vulkan REQUIRED)
else()
  find_package(Vulkan REQUIRED)
  set(VULKAN_LIB Vulkan::Vulkan)
endif()
if(NOT GLSLC)
  find_program(GLSLC glslc REQUIRED)
endif()
if(NOT RAWR_EMBED_BINARY_SCRIPT)
  include(${CMAKE_CURRENT_LIST_DIR}/../../vk_common/cmake/EmbedSpirv.cmake)
endif()

set(RAWR_NATIVE_DIR ${CMAKE_CURRENT_LIST_DIR}/../..)
set(RAWR_MF_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
set(RAWR_MF_SPV_DIR ${CMAKE_CURRENT_BINARY_DIR}/generated/multiframe)
file(MAKE_DIRECTORY ${RAWR_MF_SPV_DIR}/prod ${RAWR_MF_SPV_DIR}/trace)
set(RAWR_MF_SHADER_SOURCES
  ${CMAKE_CURRENT_LIST_DIR}/shaders/raw_normalize.comp
  ${RAWR_MF_DIR}/alignment/shaders/alignment_local5.comp
  ${RAWR_MF_DIR}/alignment/shaders/pyramid_gaussian.comp
  ${RAWR_MF_DIR}/alignment/shaders/pyramid_decimate.comp
  ${RAWR_MF_DIR}/alignment/shaders/flow_upsample.comp
  ${RAWR_MF_DIR}/alignment/shaders/block_match.comp
  ${RAWR_MF_DIR}/alignment/shaders/lk_refine.comp
  ${RAWR_MF_DIR}/alignment/shaders/flow_dense_smooth.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/kernel_estimate.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/robust_guide.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/robust_stats.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/robust_warp.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/robust_flow_scale.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/robust_photometric.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/robust_erode5.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/affine_fit.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/affine_gate.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/support_accumulate.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/accumulate.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/reference_accumulate.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/a11_finalize.comp
  ${CMAKE_CURRENT_LIST_DIR}/shaders/noise_estimate.comp
  ${RAWR_MF_DIR}/merge_wronski/shaders/fallback_chroma.comp
  ${CMAKE_CURRENT_LIST_DIR}/shaders/hot_pixel_conceal.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_prepare.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_hot_pixel.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_avg_pool.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_blur.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_upsample_align.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_correct_upsampling.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_tile_diff.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_best_tile.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_warp.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_color_diff.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_column_sum.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_mean.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_merge_weight.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_accumulate.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_finalize.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_to_rgba.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_warp_rgba.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_rms.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_mismatch.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_region_mean.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_mismatch_norm.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_forward_dft.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_merge.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_deconvolute.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_backward_dft.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_border.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_accumulate.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_shift_table.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_highlights_norm.comp)
set(RAWR_MF_SPV_OUTPUTS)
foreach(RAWR_MF_SRC IN LISTS RAWR_MF_SHADER_SOURCES)
  get_filename_component(RAWR_MF_NAME ${RAWR_MF_SRC} NAME_WE)
  set(RAWR_MF_PROD ${RAWR_MF_SPV_DIR}/prod/${RAWR_MF_NAME}.spv)
  set(RAWR_MF_TRACE ${RAWR_MF_SPV_DIR}/trace/${RAWR_MF_NAME}.spv)
  set(RAWR_MF_PROD_HDR ${RAWR_MF_SPV_DIR}/mf_${RAWR_MF_NAME}_prod.h)
  set(RAWR_MF_TRACE_HDR ${RAWR_MF_SPV_DIR}/mf_${RAWR_MF_NAME}_trace.h)
  # Per-channel robustness veto (RAWR_MIN_GATE) ships in production
  # multiframe merges; trace keeps the upstream aggregate path for forensics.
  set(RAWR_MF_PROD_EXTRA)
  if(RAWR_MF_NAME STREQUAL "robust_photometric")
    set(RAWR_MF_PROD_EXTRA -DRAWR_MIN_GATE=1)
  endif()
  add_custom_command(OUTPUT ${RAWR_MF_PROD} ${RAWR_MF_PROD_HDR}
    COMMAND ${GLSLC} --target-env=vulkan1.1 -O ${RAWR_MF_PROD_EXTRA} ${RAWR_MF_SRC} -o ${RAWR_MF_PROD}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${RAWR_MF_PROD} -DOUTPUT=${RAWR_MF_PROD_HDR} -DSYMBOL=mf_${RAWR_MF_NAME}_prod_spv -P ${RAWR_EMBED_BINARY_SCRIPT}
    DEPENDS ${RAWR_MF_SRC} VERBATIM)
  add_custom_command(OUTPUT ${RAWR_MF_TRACE} ${RAWR_MF_TRACE_HDR}
    COMMAND ${GLSLC} --target-env=vulkan1.1 -O -DRAWR_PARITY_TRACE=1 ${RAWR_MF_SRC} -o ${RAWR_MF_TRACE}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${RAWR_MF_TRACE} -DOUTPUT=${RAWR_MF_TRACE_HDR} -DSYMBOL=mf_${RAWR_MF_NAME}_trace_spv -P ${RAWR_EMBED_BINARY_SCRIPT}
    DEPENDS ${RAWR_MF_SRC} VERBATIM)
  list(APPEND RAWR_MF_SPV_OUTPUTS ${RAWR_MF_PROD} ${RAWR_MF_TRACE} ${RAWR_MF_PROD_HDR} ${RAWR_MF_TRACE_HDR})
endforeach()
add_custom_target(raw_multiframe_gpu_shaders ALL DEPENDS ${RAWR_MF_SPV_OUTPUTS})

add_library(raw_gpu_pipeline STATIC
  src/ResourceGeometry.cpp
  src/ScratchLayout.cpp
  src/ReconstructionTiling.cpp
  src/VulkanExecutor.cpp
  src/ResourceArena.cpp
  src/MultiframeRecorder.cpp
  src/HdrPlusRecorder.cpp
  src/AndroidBurstCoordinator.cpp
  src/BurstNoiseEstimate.cpp)
add_dependencies(raw_gpu_pipeline raw_multiframe_gpu_shaders)
target_include_directories(raw_gpu_pipeline
  PUBLIC include
         ${RAWR_MF_DIR}/alignment/include
         ${RAWR_MF_DIR}/merge_wronski/include
         ${RAWR_MF_DIR}/merge_hdrplus/include
         ${RAWR_NATIVE_DIR}/zsl_codec/include
         ${RAWR_NATIVE_DIR}/zsl_ring/include
  PRIVATE ${RAWR_MF_SPV_DIR})
target_link_libraries(raw_gpu_pipeline PUBLIC ${VULKAN_LIB})
target_compile_features(raw_gpu_pipeline PUBLIC cxx_std_17)
target_compile_options(raw_gpu_pipeline PRIVATE -Wall -Wextra -Wpedantic)
# Warnings are errors on Android. Host SDKs declare core entry points as
# functions, so the runtime-availability checks for API-level-dependent
# entry points (vkGet*MemoryRequirements2) trip -Wpointer-bool-conversion there.
if(ANDROID)
  target_compile_options(raw_gpu_pipeline PRIVATE -Werror)
endif()

if(RAW_GPU_PIPELINE_BUILD_TESTS)
  enable_testing()
  foreach(TEST_NAME merge_math_test hdrplus_geometry_test reconstruction_tiling_test resource_geometry_test scratch_layout_test)
    add_executable(${TEST_NAME} tests/${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE raw_gpu_pipeline)
    # The tests are assert-based; keep asserts active in every build type.
    target_compile_options(${TEST_NAME} PRIVATE -UNDEBUG)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
  endforeach()
  # GPU gate (needs a Vulkan device, MoltenVK on macOS).
  add_executable(cfa_pattern_merge_vulkan_test tests/cfa_pattern_merge_vulkan_test.cpp)
  target_include_directories(cfa_pattern_merge_vulkan_test PRIVATE ${RAWR_NATIVE_DIR}/vk_common/testing)
  target_link_libraries(cfa_pattern_merge_vulkan_test PRIVATE raw_gpu_pipeline)
  add_test(NAME cfa_pattern_merge_vulkan_test COMMAND cfa_pattern_merge_vulkan_test)
endif()
