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
  ${CMAKE_CURRENT_LIST_DIR}/shaders/noise_estimate.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_prepare.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_hot_pixel.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_avg_pool.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_blur.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_upsample_align.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_correct_upsampling.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_tile_diff.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_best_tile.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrp_finalize.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_to_rgba.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_warp_rgba.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_rms.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_mismatch.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_region_mean.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_mismatch_norm.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_seed_dft.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_forward_dft.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_merge.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_deconvolute.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_backward_dft.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_border.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_accumulate.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_shift_table.comp
  ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_highlights_norm.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_frame_prep.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_pyramid.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_kernel_blur.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_kernel_cov.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_qinv_debug.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_rcd_bound.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_ref_tiles.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_cand_select.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_block_match.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_lk.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_fill.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_robust.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_accumulate.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_finalize.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_frame_score.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_rcd_direction.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_rcd_green.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_rcd_diagonal.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_rcd_green_sites.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_rcd_export.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_lowcov_eaw.comp
  ${RAWR_MF_DIR}/merge_rexture/shaders/rx_fb_flags.comp)
# Guide positions per workgroup edge of rx_frame_prep (shader RX_PG and the recorder's dispatch must agree).
set(RAWR_RX_PREP_PG 16 CACHE STRING "rx_frame_prep workgroup tile edge")
set(RAWR_MF_SPV_OUTPUTS)
foreach(RAWR_MF_SRC IN LISTS RAWR_MF_SHADER_SOURCES)
  get_filename_component(RAWR_MF_NAME ${RAWR_MF_SRC} NAME_WE)
  set(RAWR_MF_PROD ${RAWR_MF_SPV_DIR}/prod/${RAWR_MF_NAME}.spv)
  set(RAWR_MF_TRACE ${RAWR_MF_SPV_DIR}/trace/${RAWR_MF_NAME}.spv)
  set(RAWR_MF_PROD_HDR ${RAWR_MF_SPV_DIR}/mf_${RAWR_MF_NAME}_prod.h)
  set(RAWR_MF_TRACE_HDR ${RAWR_MF_SPV_DIR}/mf_${RAWR_MF_NAME}_trace.h)
  # Per-channel robustness veto (RAWR_MIN_GATE) ships in production
  # multiframe merges; trace keeps the upstream aggregate path for forensics.
  set(RAWR_MF_INCLUDE_DEPS)
  if(RAWR_MF_NAME MATCHES "^hdrq_(forward|seed)_dft$")
    list(APPEND RAWR_MF_INCLUDE_DEPS ${RAWR_MF_DIR}/merge_hdrplus/shaders/hdrq_forward_dft.glsl)
  endif()
  if(RAWR_MF_NAME MATCHES "^rx_")
    file(GLOB RAWR_RX_GLSL ${RAWR_MF_DIR}/merge_rexture/shaders/*.glsl)
    list(APPEND RAWR_MF_INCLUDE_DEPS ${RAWR_RX_GLSL})
  endif()
  set(RAWR_MF_PROD_EXTRA)
  if(RAWR_MF_NAME STREQUAL "rx_frame_prep")
    set(RAWR_MF_PROD_EXTRA -DRX_PG=${RAWR_RX_PREP_PG})
  endif()
  add_custom_command(OUTPUT ${RAWR_MF_PROD} ${RAWR_MF_PROD_HDR}
    COMMAND ${GLSLC} --target-env=vulkan1.1 -O ${RAWR_MF_PROD_EXTRA} ${RAWR_MF_SRC} -o ${RAWR_MF_PROD}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${RAWR_MF_PROD} -DOUTPUT=${RAWR_MF_PROD_HDR} -DSYMBOL=mf_${RAWR_MF_NAME}_prod_spv -P ${RAWR_EMBED_BINARY_SCRIPT}
    DEPENDS ${RAWR_MF_SRC} ${RAWR_MF_INCLUDE_DEPS} VERBATIM)
  add_custom_command(OUTPUT ${RAWR_MF_TRACE} ${RAWR_MF_TRACE_HDR}
    COMMAND ${GLSLC} --target-env=vulkan1.1 -O -DRAWR_PARITY_TRACE=1 ${RAWR_MF_SRC} -o ${RAWR_MF_TRACE}
    COMMAND ${CMAKE_COMMAND} -DINPUT=${RAWR_MF_TRACE} -DOUTPUT=${RAWR_MF_TRACE_HDR} -DSYMBOL=mf_${RAWR_MF_NAME}_trace_spv -P ${RAWR_EMBED_BINARY_SCRIPT}
    DEPENDS ${RAWR_MF_SRC} ${RAWR_MF_INCLUDE_DEPS} VERBATIM)
  list(APPEND RAWR_MF_SPV_OUTPUTS ${RAWR_MF_PROD} ${RAWR_MF_TRACE} ${RAWR_MF_PROD_HDR} ${RAWR_MF_TRACE_HDR})
endforeach()
add_custom_target(raw_multiframe_gpu_shaders ALL DEPENDS ${RAWR_MF_SPV_OUTPUTS})

add_library(raw_gpu_pipeline STATIC
  src/ResourceGeometry.cpp
  src/ScratchLayout.cpp
  src/VulkanExecutor.cpp
  src/ResourceArena.cpp
  src/HdrPlusRecorder.cpp
  src/RextureRecorder.cpp
  src/RextureFrameScorer.cpp
  src/AndroidBurstCoordinator.cpp
  src/BurstNoiseEstimate.cpp)
add_dependencies(raw_gpu_pipeline raw_multiframe_gpu_shaders)
target_include_directories(raw_gpu_pipeline
  PUBLIC include
         ${RAWR_MF_DIR}/merge_wronski/include
         ${RAWR_MF_DIR}/merge_hdrplus/include
         ${RAWR_MF_DIR}/merge_rexture/include
         ${RAWR_NATIVE_DIR}/zsl_codec/include
         ${RAWR_NATIVE_DIR}/zsl_ring/include
  PRIVATE ${RAWR_MF_SPV_DIR})
target_link_libraries(raw_gpu_pipeline PUBLIC ${VULKAN_LIB})
target_compile_definitions(raw_gpu_pipeline PRIVATE RX_PREP_PG=${RAWR_RX_PREP_PG})
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
  foreach(TEST_NAME hdrplus_geometry_test resource_geometry_test rexture_base_selection_test)
    add_executable(${TEST_NAME} tests/${TEST_NAME}.cpp)
    target_link_libraries(${TEST_NAME} PRIVATE raw_gpu_pipeline)
    # The tests are assert-based; keep asserts active in every build type.
    target_compile_options(${TEST_NAME} PRIVATE -UNDEBUG)
    add_test(NAME ${TEST_NAME} COMMAND ${TEST_NAME})
  endforeach()
  # GPU gate (needs a Vulkan device, MoltenVK on macOS).
  add_executable(hdrplus_dft_seed_vulkan_test tests/hdrplus_dft_seed_vulkan_test.cpp)
  target_include_directories(hdrplus_dft_seed_vulkan_test PRIVATE ${RAWR_NATIVE_DIR}/vk_common/testing)
  target_link_libraries(hdrplus_dft_seed_vulkan_test PRIVATE raw_gpu_pipeline)
  add_test(NAME hdrplus_dft_seed_vulkan_test COMMAND hdrplus_dft_seed_vulkan_test)
endif()
