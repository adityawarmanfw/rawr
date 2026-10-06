cmake_minimum_required(VERSION 3.24)
project(rawrcam_native LANGUAGES C CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Rootless custom Adreno driver bootstrap. Keep this third-party target outside
# Rawr's strict warning policy and pin the reviewed upstream release. FetchContent
# also checks out libadrenotools' linkernsbypass submodule recursively.
include(FetchContent)
FetchContent_Declare(
  adrenotools_src
  GIT_REPOSITORY https://github.com/bylaws/libadrenotools.git
  GIT_TAG cc96fcaf159b06d6db8678c166cd9f2f53ff4b6d
  GIT_SUBMODULES_RECURSE TRUE)

# libadrenotools v1.0 pins an older liblinkernsbypass revision that uses the
# compile-time PAGE_SIZE macro. Android NDK r28+ intentionally stops defining
# PAGE_SIZE for 64-bit ABIs so code remains compatible with runtime 4 KiB/16 KiB
# page sizes. Current liblinkernsbypass upstream fixes these exact uses with
# getpagesize(). Apply that narrow upstream compatibility fix to the fetched
# v1.0 source while retaining a reproducible libadrenotools release pin.
FetchContent_GetProperties(adrenotools_src)
if(NOT adrenotools_src_POPULATED)
  FetchContent_Populate(adrenotools_src)
endif()
# libadrenotools v1.0 predates Android's NDK r28+ flexible-page-size changes.
# Current upstream replaces compile-time PAGE_SIZE in BOTH of these stale v1.0
# sources with runtime getpagesize(). Apply that exact compatibility transform
# before adding the third-party project, while keeping libadrenotools itself pinned.
set(RAWR_ADRENOTOOLS_PAGE_SIZE_SOURCES
    "${adrenotools_src_SOURCE_DIR}/lib/linkernsbypass/android_linker_ns.cpp"
    "${adrenotools_src_SOURCE_DIR}/src/bcenabler.cpp")
foreach(RAWR_PAGE_SIZE_SOURCE IN LISTS RAWR_ADRENOTOOLS_PAGE_SIZE_SOURCES)
  if(NOT EXISTS "${RAWR_PAGE_SIZE_SOURCE}")
    message(FATAL_ERROR "libadrenotools compatibility source missing: ${RAWR_PAGE_SIZE_SOURCE}")
  endif()
  file(READ "${RAWR_PAGE_SIZE_SOURCE}" RAWR_PAGE_SIZE_TEXT)
  string(FIND "${RAWR_PAGE_SIZE_TEXT}" "PAGE_SIZE" RAWR_PAGE_SIZE_POS)
  if(NOT RAWR_PAGE_SIZE_POS EQUAL -1)
    string(REPLACE "PAGE_SIZE" "getpagesize()"
           RAWR_PAGE_SIZE_TEXT "${RAWR_PAGE_SIZE_TEXT}")
  elseif(NOT RAWR_PAGE_SIZE_TEXT MATCHES "getpagesize\(\)")
    message(FATAL_ERROR "Unexpected page-size implementation: ${RAWR_PAGE_SIZE_SOURCE}")
  endif()

  # getpagesize() is declared by <unistd.h>. Current upstream bcenabler.cpp
  # includes it, but the v1.0 release predates that include. Backport the
  # declaration together with the PAGE_SIZE -> getpagesize() change.
  if(RAWR_PAGE_SIZE_TEXT MATCHES "getpagesize\(\)" AND
     NOT RAWR_PAGE_SIZE_TEXT MATCHES "#include[ \t]*<unistd\\.h>")
    string(FIND "${RAWR_PAGE_SIZE_TEXT}" "\n" RAWR_FIRST_NEWLINE)
    if(RAWR_FIRST_NEWLINE EQUAL -1)
      message(FATAL_ERROR "Unexpected source layout: ${RAWR_PAGE_SIZE_SOURCE}")
    endif()
    math(EXPR RAWR_AFTER_FIRST_NEWLINE "${RAWR_FIRST_NEWLINE} + 1")
    string(SUBSTRING "${RAWR_PAGE_SIZE_TEXT}" 0 ${RAWR_AFTER_FIRST_NEWLINE} RAWR_SOURCE_HEAD)
    string(SUBSTRING "${RAWR_PAGE_SIZE_TEXT}" ${RAWR_AFTER_FIRST_NEWLINE} -1 RAWR_SOURCE_TAIL)
    set(RAWR_PAGE_SIZE_TEXT
        "${RAWR_SOURCE_HEAD}#include <unistd.h>\n${RAWR_SOURCE_TAIL}")
  endif()

  file(WRITE "${RAWR_PAGE_SIZE_SOURCE}" "${RAWR_PAGE_SIZE_TEXT}")
endforeach()
add_subdirectory("${adrenotools_src_SOURCE_DIR}" "${adrenotools_src_BINARY_DIR}")

add_subdirectory(../../../../native/tinydng ${CMAKE_CURRENT_BINARY_DIR}/tinydng)

# Rawr-owned targets remain strict; do not impose -Werror on the pinned third-party code.
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)
add_compile_options(-Wall -Wextra -Wpedantic -Werror)

# Rawr owns Vulkan dispatch so system vs app-local Turnip is selected before VkInstance creation.
# Vulkan calls are dispatched through VulkanDispatch.cpp (VK_NO_PROTOTYPES), so
# vendored static libraries only need a non-exported placeholder dependency at
# configure time.  Make it IMPORTED so their install(EXPORT ...) sets do not
# acquire a Rawr-local target that CMake then requires them to export.
add_library(vulkan INTERFACE IMPORTED GLOBAL)
set(VULKAN_LIB vulkan CACHE STRING "Rawr Vulkan dispatch placeholder" FORCE)
set(RAW_STATS_VULKAN_LIB vulkan CACHE STRING "Rawr Vulkan dispatch placeholder" FORCE)
set(IMAGE_SCOPES_VULKAN_LIB vulkan CACHE STRING "Rawr Vulkan dispatch placeholder" FORCE)
set(FCC_VULKAN_LIB vulkan CACHE STRING "Rawr Vulkan dispatch placeholder" FORCE)
set(MONITORING_OVERLAYS_VULKAN_LINK vulkan CACHE STRING "Rawr Vulkan dispatch placeholder" FORCE)
add_compile_definitions(VK_NO_PROTOTYPES)
add_compile_options($<$<COMPILE_LANGUAGE:C,CXX>:-include${CMAKE_CURRENT_SOURCE_DIR}/vulkan/VulkanDispatch.h>)

find_library(ANDROID_LIB android REQUIRED)
find_library(LOG_LIB log REQUIRED)
find_library(MEDIA_LIB mediandk REQUIRED)
find_library(AAUDIO_LIB aaudio REQUIRED)
find_library(CAMERA2NDK_LIB camera2ndk REQUIRED)

# Vendored libjpeg-turbo 3.2.0. Upstream explicitly rejects add_subdirectory(),
# so build its static TurboJPEG target as a nested CMake project using the same
# Android toolchain/ABI.  This keeps upstream source unmodified and preserves
# its architecture-specific SIMD selection.
include(ExternalProject)
set(LIBJPEG_TURBO_SOURCE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/libjpeg-turbo)
set(LIBJPEG_TURBO_BINARY_DIR ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-build)
ExternalProject_Add(libjpeg_turbo_external
  SOURCE_DIR ${LIBJPEG_TURBO_SOURCE_DIR}
  BINARY_DIR ${LIBJPEG_TURBO_BINARY_DIR}
  CMAKE_ARGS
    "-DCMAKE_MAKE_PROGRAM:FILEPATH=${CMAKE_MAKE_PROGRAM}"
    -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE}
    -DANDROID_ABI=${ANDROID_ABI}
    -DANDROID_PLATFORM=${ANDROID_PLATFORM}
    -DCMAKE_BUILD_TYPE=${CMAKE_BUILD_TYPE}
    -DENABLE_SHARED=OFF
    -DENABLE_STATIC=ON
    -DWITH_TURBOJPEG=ON
    -DWITH_TOOLS=OFF
    -DWITH_TESTS=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${LIBJPEG_TURBO_BINARY_DIR}/libturbojpeg.a
  USES_TERMINAL_CONFIGURE TRUE
  USES_TERMINAL_BUILD TRUE)
add_library(rawrcam_turbojpeg STATIC IMPORTED GLOBAL)
set_target_properties(rawrcam_turbojpeg PROPERTIES
  IMPORTED_LOCATION ${LIBJPEG_TURBO_BINARY_DIR}/libturbojpeg.a
  INTERFACE_INCLUDE_DIRECTORIES ${LIBJPEG_TURBO_SOURCE_DIR}/src)
add_dependencies(rawrcam_turbojpeg libjpeg_turbo_external)

if(CMAKE_HOST_SYSTEM_NAME STREQUAL "Darwin")
  set(RAWR_SHADER_HOST darwin-x86_64)
elseif(CMAKE_HOST_SYSTEM_NAME STREQUAL "Linux")
  set(RAWR_SHADER_HOST linux-x86_64)
else()
  message(FATAL_ERROR "Supported build hosts are macOS and Linux")
endif()
find_program(GLSLC glslc PATHS "${CMAKE_ANDROID_NDK}/shader-tools/${RAWR_SHADER_HOST}"
  NO_DEFAULT_PATH REQUIRED)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN})
include(../../../../native/vk_common/cmake/EmbedSpirv.cmake)
set(EMBED_SPIRV_OUTPUT_DIR ${GEN})
# Shared native helpers: borrowed-handle Vulkan utilities and RAW conventions.
add_subdirectory(../../../../native/vk_common ${CMAKE_CURRENT_BINARY_DIR}/vk_common)
add_subdirectory(../../../../native/shader_common ${CMAKE_CURRENT_BINARY_DIR}/shader_common)
set(RAW_PREVIEW_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/raw_preview/shaders)
set(TONEMAP_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/tonemap_engine/shaders)
if(NOT TARGET raw_highlight::shader_contract)
  add_subdirectory(../../../../native/raw_highlight ${CMAKE_CURRENT_BINARY_DIR}/raw_highlight)
endif()
get_target_property(RAW_HIGHLIGHT_SHADER_DIR raw_highlight_shader_contract INTERFACE_RAW_HIGHLIGHT_SHADER_DIR)
if(NOT RAW_HIGHLIGHT_SHADER_DIR)
  message(FATAL_ERROR "raw_highlight shader contract did not publish its shader directory")
endif()

function(embed_shader NAME SRC SYMBOL)
  # Per-shader glslc flags via EMBED_FLAGS_<NAME> (list). Empty = legacy
  # unoptimized compile. The UltraHDR gain map needs -O --target-env: its
  # uniform clip branch trips Adreno's compiler in unoptimized form
  # (vkCreateComputePipelines UNKNOWN), while the optimized module is the
  # same one validated on host.
  embed_spirv(${NAME} ${SRC} ${SYMBOL}
    FLAGS ${EMBED_FLAGS_${NAME}} -I${RAWR_SHADER_COMMON_DIR} -I${RAW_PREVIEW_SHADER_DIR} -I${RAW_HIGHLIGHT_SHADER_DIR} -I${TONEMAP_SHADER_DIR} -I${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/video_pipeline/shaders -I${CMAKE_CURRENT_SOURCE_DIR}/develop/highlight
    DEPENDS ${ARGN})
  set(${NAME}_HDR ${${NAME}_HDR} PARENT_SCOPE)
endfunction()

set(RAW_SHADER ${RAW_PREVIEW_SHADER_DIR}/raw_preview.comp)
set(RAW_POST_RGB_HIGHLIGHT ${RAW_HIGHLIGHT_SHADER_DIR}/raw_highlight_post_rgb.glsl)
set(RAW_STATE_SHADER ${RAW_PREVIEW_SHADER_DIR}/raw_preview_cfa_state.comp)
set(RAW_HL_SEED_SHADER ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_seed.comp)
set(RAW_HL_PROPAGATE_SHADER ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_propagate.comp)
set(RAW_HL_SMOOTH_SHADER ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_smooth.comp)
set(RAW_HL_APPLY_SHADER ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_apply_inplace.comp)
set(RAW_HL_SAMPLE ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_sample.glsl)
# Preview works on the half-resolution image (scale 1); the still/video
# variants (scale 2) are built by native/post_demosaic.
set(EMBED_FLAGS_raw_preview_highlight_guide_seed -DRAWR_HL_SCALE=1)
set(EMBED_FLAGS_raw_preview_highlight_apply -DRAWR_HL_SCALE=1)
set(RAW_LSC_SHADER ${RAWR_SHADER_COMMON_DIR}/lens_shading.glsl)
set(RAW_CFA_COMMON ${RAWR_SHADER_COMMON_DIR}/cfa_common.glsl)
set(TONEMAP_SHADER ${TONEMAP_SHADER_DIR}/tonemap.comp)
set(TONEMAP_CST_SHADER ${TONEMAP_SHADER_DIR}/color/cst.glsl ${TONEMAP_SHADER_DIR}/color/gamut_matrices.inc)
set(TONEMAP_LUT_SHADER ${TONEMAP_SHADER_DIR}/lut/lut3d.glsl)
embed_shader(raw_preview ${RAW_SHADER} raw_preview_spv ${RAW_POST_RGB_HIGHLIGHT} ${RAW_LSC_SHADER} ${RAW_CFA_COMMON})
embed_shader(raw_zsl_codec_analyze ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/zsl_codec/raw_zsl_codec_analyze.comp raw_zsl_codec_analyze_spv)
embed_shader(raw_zsl_codec_pack ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/zsl_codec/raw_zsl_codec_pack.comp raw_zsl_codec_pack_spv)
embed_shader(raw_zsl_codec_verify ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/zsl_codec/raw_zsl_codec_verify.comp raw_zsl_codec_verify_spv)
embed_shader(raw_zsl_codec_decode ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/zsl_codec/raw_zsl_codec_decode.comp raw_zsl_codec_decode_spv)
embed_shader(raw_preview_cfa_state ${RAW_STATE_SHADER} raw_preview_cfa_state_spv ${RAW_POST_RGB_HIGHLIGHT} ${RAW_LSC_SHADER} ${RAW_CFA_COMMON})
embed_shader(raw_preview_highlight_guide_seed ${RAW_HL_SEED_SHADER} raw_preview_highlight_guide_seed_spv ${RAW_POST_RGB_HIGHLIGHT})
embed_shader(raw_preview_highlight_guide_propagate ${RAW_HL_PROPAGATE_SHADER} raw_preview_highlight_guide_propagate_spv)
embed_shader(raw_preview_highlight_guide_smooth ${RAW_HL_SMOOTH_SHADER} raw_preview_highlight_guide_smooth_spv)
embed_shader(raw_preview_highlight_apply ${RAW_HL_APPLY_SHADER} raw_preview_highlight_apply_spv ${RAW_POST_RGB_HIGHLIGHT} ${RAW_HL_SAMPLE})
# Preview Inpaint Opposed runs on white-balanced RGB; the tone tap is the same shader stills use.
set(EMBED_FLAGS_raw_preview_coloropp -DRAWR_COLOROPP_POST_WB=1)
embed_shader(raw_preview_coloropp ${RAW_HIGHLIGHT_SHADER_DIR}/coloropp.comp raw_preview_coloropp_spv ${RAW_LSC_SHADER} ${RAW_CFA_COMMON})
embed_shader(raw_preview_coloropp_tone ${RAW_HIGHLIGHT_SHADER_DIR}/coloropp_tone.comp raw_preview_coloropp_tone_spv)
embed_shader(tonemap ${TONEMAP_SHADER} tonemap_spv ${TONEMAP_CST_SHADER} ${TONEMAP_LUT_SHADER})
set(EMBED_FLAGS_tonemap_video_linear -DRAWR_TONEMAP_OUTPUT_RGBA16F=1 -DRAWR_TONEMAP_VIDEO_MONITOR=1 -DLOCAL_X=8 -DLOCAL_Y=8)
embed_shader(tonemap_video_linear ${TONEMAP_SHADER} tonemap_video_linear_spv ${TONEMAP_CST_SHADER} ${TONEMAP_LUT_SHADER})
# 8-bit recording writes dithered RGBA8 straight into the encoder Surface.
set(EMBED_FLAGS_tonemap_video_rgba8 -DRAWR_TONEMAP_VIDEO_MONITOR=1 -DLOCAL_X=8 -DLOCAL_Y=8)
embed_shader(tonemap_video_rgba8 ${TONEMAP_SHADER} tonemap_video_rgba8_spv ${TONEMAP_CST_SHADER} ${TONEMAP_LUT_SHADER})
# 10-bit recording into a packed A2B10G10R10 encoder Surface (4 bytes per pixel).
set(EMBED_FLAGS_tonemap_video_rgb10a2 -DRAWR_TONEMAP_OUTPUT_RGB10A2=1 -DRAWR_TONEMAP_VIDEO_MONITOR=1 -DLOCAL_X=8 -DLOCAL_Y=8)
embed_shader(tonemap_video_rgb10a2 ${TONEMAP_SHADER} tonemap_video_rgb10a2_spv ${TONEMAP_CST_SHADER} ${TONEMAP_LUT_SHADER})
# Texture-backed Neutral retains tetrahedral interpolation. Runtime keeps the
# buffer variants for custom LUT chains and devices without measured gains.
foreach(VIDEO_FORMAT linear rgba8 rgb10a2)
  set(EMBED_FLAGS_tonemap_video_${VIDEO_FORMAT}_texture ${EMBED_FLAGS_tonemap_video_${VIDEO_FORMAT}} -DRAWR_TONEMAP_NEUTRAL_TEXTURE=1)
  embed_shader(tonemap_video_${VIDEO_FORMAT}_texture ${TONEMAP_SHADER} tonemap_video_${VIDEO_FORMAT}_texture_spv ${TONEMAP_CST_SHADER} ${TONEMAP_LUT_SHADER})
endforeach()
embed_shader(sensor_clip_pack ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/raw_demosaic/common/shaders/sensor_clip_pack.comp sensor_clip_pack_spv ${RAW_LSC_SHADER} ${RAW_CFA_COMMON})
embed_shader(replay_tagged_decode ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/replay/shaders/replay_tagged_decode.comp replay_tagged_decode_spv)
embed_shader(monitor_raw_state ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/monitoring_overlays/shaders/raw_state_overlay.comp monitor_raw_state_spv)
embed_shader(monitor_focus ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/monitoring_overlays/shaders/focus_peaking.comp monitor_focus_spv)
embed_shader(monitor_false_color ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/monitoring_overlays/shaders/false_color.comp monitor_false_color_spv)
embed_shader(monitor_tonemap_shadow ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/monitoring_overlays/shaders/tonemap_shadow.comp monitor_tonemap_shadow_spv)
embed_shader(monitor_combined ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/monitoring_overlays/shaders/combined_overlay.comp monitor_combined_spv)
embed_shader(present_vert ${CMAKE_CURRENT_SOURCE_DIR}/presentation/shaders/present.vert present_vert_spv)
embed_shader(present_frag ${CMAKE_CURRENT_SOURCE_DIR}/presentation/shaders/present.frag present_frag_spv)
embed_shader(encoder_ab_pattern ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/encoder_ab_pattern.frag encoder_ab_pattern_spv)
embed_shader(raw_integrity_probe ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/raw_integrity_probe.comp raw_integrity_probe_spv)
embed_shader(raw_zero_copy_full_compare ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/raw_zero_copy_full_compare.comp raw_zero_copy_full_compare_spv)
embed_shader(raw_zero_copy_paths ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/raw_zero_copy_paths.comp raw_zero_copy_paths_spv)
embed_shader(raw_zero_copy_external_compare ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/raw_zero_copy_external_compare.comp raw_zero_copy_external_compare_spv)
embed_shader(raw_zero_copy_buffer_preview ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/raw_zero_copy_buffer_preview.comp raw_zero_copy_buffer_preview_spv ${RAW_POST_RGB_HIGHLIGHT})
embed_shader(raw_zero_copy_buffer_import ${CMAKE_CURRENT_SOURCE_DIR}/vulkan/shaders/raw_zero_copy_buffer_import.comp raw_zero_copy_buffer_import_spv)
embed_shader(diagnostic_linear_pattern ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/diagnostic_linear_pattern.comp diagnostic_linear_pattern_spv)
embed_shader(diagnostic_tone_pattern ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/diagnostic_tone_pattern.comp diagnostic_tone_pattern_spv)
embed_shader(diagnostic_raw_copy ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/diagnostic_raw_copy.comp diagnostic_raw_copy_spv)
embed_shader(diagnostic_raw_storage_viz ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/diagnostic_raw_storage_viz.comp diagnostic_raw_storage_viz_spv)
embed_shader(diagnostic_raw_sampled_viz ${CMAKE_CURRENT_SOURCE_DIR}/diagnostics/probes/shaders/diagnostic_raw_sampled_viz.comp diagnostic_raw_sampled_viz_spv)

set(RAW_PREVIEW_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/raw_preview ${CMAKE_CURRENT_BINARY_DIR}/raw_preview)

# Raw demosaic home: standalone RCD, standalone VNG4, Dual blend.
# One add_subdirectory provides rcd::rcd, vng4::vng4, dual::dual_demosaic.
set(RAW_DEMOSAIC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/raw_demosaic)
set(RCD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RCD_ENABLE_WARNINGS ON CACHE BOOL "" FORCE)
set(VNG4_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VNG4_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(DUAL_BUILD_VALIDATION OFF CACHE BOOL "" FORCE)
set(RAW_DEMOSAIC_EMBED_SHADERS ON CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/raw_demosaic ${CMAKE_CURRENT_BINARY_DIR}/raw_demosaic)


# Frozen one-pass RawTherapee false-color correction. Validation executables are
# standalone-package gates and stay out of the app build.
set(FCC_BUILD_VALIDATION OFF CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/false_color_correction ${CMAKE_CURRENT_BINARY_DIR}/false_color_correction)

set(TONEMAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TONEMAP_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/tonemap_engine ${CMAKE_CURRENT_BINARY_DIR}/tonemap_engine)

# UltraHDR gain map (GPU-direct): HDR R16F + SDR R8 -> RGB recovery map.
set(GAINMAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GAINMAP_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/gainmap ${CMAKE_CURRENT_BINARY_DIR}/gainmap)
set(GAINMAP_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/gainmap/shaders)
set(EMBED_FLAGS_gainmap --target-env=vulkan1.1 -O)
embed_shader(gainmap ${GAINMAP_SHADER_DIR}/gainmap.comp gainmap_spv)
set(EMBED_FLAGS_gainmap_blur --target-env=vulkan1.1 -O)
embed_shader(gainmap_blur ${GAINMAP_SHADER_DIR}/gainmap_blur.comp gainmap_blur_spv)

# spektrafilm film simulation: record-only stage, no standalone tools in app.
set(SPEKTRA_FILM_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/spektrafilm ${CMAKE_CURRENT_BINARY_DIR}/spektrafilm)
set(SPEKTRA_FILM_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/spektrafilm/shaders)
embed_shader(film_input ${SPEKTRA_FILM_SHADER_DIR}/spektra_input.comp film_input_spv)
embed_shader(film_exposure ${SPEKTRA_FILM_SHADER_DIR}/SpektraFilmExposure.comp film_exposure_spv)
embed_shader(film_develop ${SPEKTRA_FILM_SHADER_DIR}/SpektraCurveDevelop.comp film_develop_spv)
embed_shader(film_grain ${SPEKTRA_FILM_SHADER_DIR}/SpektraGrain.comp film_grain_spv)
embed_shader(film_dir ${SPEKTRA_FILM_SHADER_DIR}/SpektraDir.comp film_dir_spv)
embed_shader(film_halation ${SPEKTRA_FILM_SHADER_DIR}/SpektraHalation.comp film_halation_spv)
embed_shader(film_diffusion ${SPEKTRA_FILM_SHADER_DIR}/SpektraDiffusion.comp film_diffusion_spv)
embed_shader(film_scanner ${SPEKTRA_FILM_SHADER_DIR}/SpektraScannerPost.comp film_scanner_spv)
embed_shader(film_printscan ${SPEKTRA_FILM_SHADER_DIR}/SpektraPrintScan.comp film_printscan_spv)
embed_shader(film_output ${SPEKTRA_FILM_SHADER_DIR}/spektra_output.comp film_output_spv)
embed_shader(film_output_half ${SPEKTRA_FILM_SHADER_DIR}/spektra_output_half.comp film_output_half_spv)
embed_shader(film_boost_milestone ${SPEKTRA_FILM_SHADER_DIR}/spektra_boost_milestone.comp film_boost_milestone_spv)
embed_shader(film_glow_ratio ${SPEKTRA_FILM_SHADER_DIR}/spektra_glow_ratio.comp film_glow_ratio_spv)

# raw_denoise owns the profiled wavelet denoise (GPU tiled image pipeline).
# Tests stay package-side (Python oracle + MoltenVK dump runner).
set(RAW_DENOISE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RAW_DENOISE_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/raw_denoise ${CMAKE_CURRENT_BINARY_DIR}/raw_denoise)

# galosh owns the GALOSH-RAW Bayer denoise (blind, training-free, P1a).
# Tests stay package-side (parity harness + MoltenVK smoke runner).
set(GALOSH_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/galosh ${CMAKE_CURRENT_BINARY_DIR}/galosh)

# Post-demosaic stages (stills, video, renderer) and the video RAW stage.
add_subdirectory(../../../../native/post_demosaic ${CMAKE_CURRENT_BINARY_DIR}/post_demosaic)
add_subdirectory(../../../../native/video_pipeline ${CMAKE_CURRENT_BINARY_DIR}/video_pipeline)
add_subdirectory(../../../../native/raw_ingress ${CMAKE_CURRENT_BINARY_DIR}/raw_ingress)
set(GALOSH_SHADER_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../../../../native/galosh/shaders)
# Single source of truth: native/galosh/shaders/SHADER_LIST. RAW set =
# o32_* + app-authored galosh_bridge_*; YUV set = yuv_* +
# app-authored galosh_yuv_bridge_* (P1b linear entry; the sRGB-wrap
# yuv_srgb2ycc/yuv_ycc2srgb are superseded by the bridges and excluded).
# Also excluded: upstream kernels the port marks optional and never
# dispatches (GaloshRawPipeline.cpp kKernels).
set(GALOSH_UNUSED_KERNELS o32_pad_2d_edge o32_inverse_wht_dark_gat o32_sigma_per_cfa)
file(STRINGS ${GALOSH_SHADER_DIR}/SHADER_LIST GALOSH_SHADER_LINES)
set(GALOSH_RAW_KERNELS "")
foreach(LINE IN LISTS GALOSH_SHADER_LINES)
  if(LINE IN_LIST GALOSH_UNUSED_KERNELS)
    continue()
  endif()
  if(LINE MATCHES "^(o32_|yuv_|galosh_)" AND NOT LINE MATCHES "srgb")
    list(APPEND GALOSH_RAW_KERNELS ${LINE})
  endif()
endforeach()
set(GALOSH_RAW_GENERATED_HEADERS "")
foreach(K IN LISTS GALOSH_RAW_KERNELS)
  set(EMBED_FLAGS_galosh_${K} -O --target-env=vulkan1.2)
  embed_shader(galosh_${K} ${GALOSH_SHADER_DIR}/${K}.comp galosh_${K}_spv)
  set(HDRVAR galosh_${K}_HDR)
  list(APPEND GALOSH_RAW_GENERATED_HEADERS ${${HDRVAR}})
endforeach()

set(ZSL_CODEC_GENERATED_HEADERS ${raw_zsl_codec_analyze_HDR} ${raw_zsl_codec_pack_HDR} ${raw_zsl_codec_verify_HDR} ${raw_zsl_codec_decode_HDR})
set(ZSL_CODEC_GENERATED_INCLUDE_DIR ${GEN})
add_custom_target(raw_zsl_codec_shaders
  DEPENDS ${ZSL_CODEC_GENERATED_HEADERS})
add_subdirectory(../../../../native/zsl_codec ${CMAKE_CURRENT_BINARY_DIR}/zsl_codec)
add_dependencies(zsl_codec raw_zsl_codec_shaders)
add_subdirectory(../../../../native/zsl_ring ${CMAKE_CURRENT_BINARY_DIR}/zsl_ring)
add_subdirectory(../../../../native/zsl_container ${CMAKE_CURRENT_BINARY_DIR}/zsl_container)

# Frozen-v18 multiframe stack (native/multiframe): executor with the
# alignment/merge shaders, output projection, burst sharpness scoring. Shader
# execution is app-orchestrated so all modules share RawrCam's VkDevice,
# queue, command buffers, and synchronization authority. Tests stay
# package-side.
set(RAW_SHARPNESS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RAW_SHARPNESS_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/multiframe ${CMAKE_CURRENT_BINARY_DIR}/multiframe)

set(IMAGE_SCOPES_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(IMAGE_SCOPES_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(IMAGE_SCOPES_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/image_scopes ${CMAKE_CURRENT_BINARY_DIR}/image_scopes)

set(MONITORING_OVERLAYS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(MONITORING_OVERLAYS_BUILD_ANDROID_MILESTONE OFF CACHE BOOL "" FORCE)
set(MONITORING_OVERLAYS_COMPILE_SHADERS OFF CACHE BOOL "" FORCE)
set(MONITORING_OVERLAYS_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory(../../../../native/monitoring_overlays ${CMAKE_CURRENT_BINARY_DIR}/monitoring_overlays)

# DNG capture writes via the vendored TinyDNG streaming writer (C11); the
# adapter/async writer stay behind a C++20 TU for consistency with history.
add_library(rawrcam_dng_capture STATIC
  encoding/dng/DngExif.cpp
  encoding/dng/DngGainMapOpcode.cpp
  encoding/dng/DngMetadataAdapter.cpp
  encoding/dng/DngPixelPayload.cpp
  encoding/dng/DngProvenance.cpp
  encoding/dng/DngCaptureWriter.cpp)
target_compile_features(rawrcam_dng_capture PRIVATE cxx_std_20)
target_include_directories(rawrcam_dng_capture PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ../../../../native/tinydng)
target_link_libraries(rawrcam_dng_capture PRIVATE rawr_tinydng)
target_compile_options(rawrcam_dng_capture PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(rawrcam_backend_options INTERFACE)
target_include_directories(rawrcam_backend_options INTERFACE ${GEN} ${CMAKE_CURRENT_SOURCE_DIR} ../../../../native/libjpeg-turbo/src ../../../../native/raw_preview/include ../../../../native/tonemap_engine/include ../../../../native/gainmap/include ../../../../native/spektrafilm/include ../../../../native/raw_denoise/include ../../../../native/galosh/include ../../../../native/image_scopes/include ../../../../native/monitoring_overlays/include ../../../../native/false_color_correction/include ../../../../native/zsl_codec/include ../../../../native/zsl_ring/include ../../../../native/zsl_container/include)
target_link_libraries(rawrcam_backend_options INTERFACE raw_demosaic::embedded_shaders rawr::post_demosaic rawr::video_pipeline rawr::raw_ingress rawr::vk_common rawr::shader_common raw_highlight::raw_highlight rawr_tinydng adrenotools raw_preview::raw_preview rcd::rcd vng4::vng4 dual::dual_demosaic quadfix::quadfix fcc::false_color_correction TonemapEngine::Vulkan Gainmap::Compute spektrafilm::spektrafilm raw_sharpness::raw_sharpness raw_denoise::raw_denoise galosh::galosh image_scopes::image_scopes monitoring_overlays::vulkan rawr::zsl_codec rawr::zsl_ring rawr::zsl_container raw_gpu_pipeline raw_multiframe_output  rawrcam_turbojpeg ${VULKAN_LIB} ${ANDROID_LIB} ${LOG_LIB} ${MEDIA_LIB} ${AAUDIO_LIB} ${CAMERA2NDK_LIB} ${CMAKE_DL_LIBS})
add_custom_target(rawrcam_shader_headers DEPENDS ${raw_preview_HDR} ${raw_preview_cfa_state_HDR} ${raw_preview_highlight_guide_seed_HDR} ${raw_preview_highlight_guide_propagate_HDR} ${raw_preview_highlight_guide_smooth_HDR} ${raw_preview_highlight_apply_HDR} ${raw_preview_coloropp_HDR} ${raw_preview_coloropp_tone_HDR} ${raw_zsl_codec_analyze_HDR} ${raw_zsl_codec_pack_HDR} ${raw_zsl_codec_verify_HDR} ${raw_zsl_codec_decode_HDR} ${monitor_raw_state_HDR} ${monitor_focus_HDR} ${monitor_false_color_HDR} ${monitor_tonemap_shadow_HDR} ${monitor_combined_HDR} ${tonemap_HDR} ${tonemap_video_linear_HDR} ${tonemap_video_rgba8_HDR} ${tonemap_video_rgb10a2_HDR} ${tonemap_video_linear_texture_HDR} ${tonemap_video_rgba8_texture_HDR} ${tonemap_video_rgb10a2_texture_HDR} ${video_demosaic_HDR} ${film_input_HDR} ${film_exposure_HDR} ${film_develop_HDR} ${film_grain_HDR} ${film_dir_HDR} ${film_halation_HDR} ${film_diffusion_HDR} ${film_scanner_HDR} ${film_printscan_HDR} ${film_output_HDR} ${film_output_half_HDR} ${film_boost_milestone_HDR} ${film_glow_ratio_HDR} ${sensor_clip_pack_HDR} ${replay_tagged_decode_HDR} ${present_vert_HDR} ${present_frag_HDR} ${encoder_ab_pattern_HDR} ${raw_integrity_probe_HDR} ${raw_zero_copy_full_compare_HDR} ${raw_zero_copy_paths_HDR} ${raw_zero_copy_external_compare_HDR} ${raw_zero_copy_buffer_preview_HDR} ${raw_zero_copy_buffer_import_HDR} ${diagnostic_linear_pattern_HDR} ${diagnostic_tone_pattern_HDR} ${diagnostic_raw_copy_HDR} ${diagnostic_raw_storage_viz_HDR} ${diagnostic_raw_sampled_viz_HDR} ${GALOSH_RAW_GENERATED_HEADERS} ${gainmap_HDR} ${gainmap_blur_HDR})

function(rawrcam_module name)
  cmake_parse_arguments(MODULE "" "DIRECTORY" "DEPENDS;SOURCES" ${ARGN})
  if(MODULE_SOURCES)
    add_library(rawrcam_${name} STATIC ${MODULE_SOURCES})
    set_target_properties(rawrcam_${name} PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_link_libraries(rawrcam_${name} PUBLIC rawrcam_backend_options)
    foreach(dependency IN LISTS MODULE_DEPENDS)
      target_link_libraries(rawrcam_${name} PUBLIC rawrcam_${dependency})
    endforeach()
    add_dependencies(rawrcam_${name} rawrcam_shader_headers)
  else()
    add_library(rawrcam_${name} INTERFACE)
    target_link_libraries(rawrcam_${name} INTERFACE rawrcam_backend_options)
    foreach(dependency IN LISTS MODULE_DEPENDS)
      target_link_libraries(rawrcam_${name} INTERFACE rawrcam_${dependency})
    endforeach()
  endif()
endfunction()
include(cmake/CameraProfiles.cmake)
rawr_embed_camera_profiles(${GEN}/camera/BuiltInCameraProfiles.inc)
include(cmake/NativeModules.cmake)
target_link_libraries(rawrcam_encoding PUBLIC rawrcam_dng_capture)

add_library(rawrcam_native SHARED
  jni/RendererJni.cpp
  jni/SessionEngineApi.cpp
  jni/NativeEngineJni.cpp
  jni/DevelopSettingsReader.cpp
  jni/VideoGpuProbeJni.cpp
  jni/EncoderAbProbeJni.cpp
  jni/NativeAvRecorderJni.cpp
)
target_link_libraries(rawrcam_native PRIVATE rawrcam_backend_options rawrcam_camera rawrcam_capture rawrcam_color rawrcam_develop rawrcam_diagnostics_logging rawrcam_encoding rawrcam_monitoring rawrcam_renderer rawrcam_session rawrcam_tonemap rawrcam_video rawrcam_vulkan)
add_dependencies(rawrcam_native rawrcam_shader_headers)
# libadrenotools owns its runtime hook targets. Do not name those private
# implementation targets here: their availability differs by upstream build
# configuration/version, and FetchContent already includes normal ALL targets.


# CPU image loops must remain optimized in debug builds used for device validation.
set_source_files_properties(renderer/LinearResample.cpp renderer/DngSource.cpp renderer/RendererEngine.cpp PROPERTIES COMPILE_OPTIONS "-O3")
