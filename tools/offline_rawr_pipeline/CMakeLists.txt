cmake_minimum_required(VERSION 3.22)
project(rawr_offline_pipeline LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(RAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

set(RCD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VNG4_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(VNG4_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(DUAL_BUILD_VALIDATION OFF CACHE BOOL "" FORCE)
add_subdirectory("${RAWR_ROOT}/native/raw_demosaic" "${CMAKE_CURRENT_BINARY_DIR}/raw_demosaic")
set(RAW_DENOISE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${RAWR_ROOT}/native/raw_denoise" "${CMAKE_CURRENT_BINARY_DIR}/raw_denoise")
add_subdirectory("${RAWR_ROOT}/native/tinydng" "${CMAKE_CURRENT_BINARY_DIR}/tinydng")
add_subdirectory("${RAWR_ROOT}/native/vk_common" "${CMAKE_CURRENT_BINARY_DIR}/vk_common")
add_subdirectory("${RAWR_ROOT}/native/shader_common" "${CMAKE_CURRENT_BINARY_DIR}/shader_common")
add_subdirectory("${RAWR_ROOT}/native/raw_highlight" "${CMAKE_CURRENT_BINARY_DIR}/raw_highlight")

set(FCC_BUILD_VALIDATION OFF CACHE BOOL "" FORCE)
add_subdirectory("${RAWR_ROOT}/native/false_color_correction" "${CMAKE_CURRENT_BINARY_DIR}/false_color_correction")
# Post-demosaic stages and their embedded SPIR-V, built exactly as in the app.
add_subdirectory("${RAWR_ROOT}/native/post_demosaic" "${CMAKE_CURRENT_BINARY_DIR}/post_demosaic")

set(TONEMAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(TONEMAP_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory("${RAWR_ROOT}/native/tonemap_engine" "${CMAKE_CURRENT_BINARY_DIR}/tonemap_engine")

set(GAINMAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(GAINMAP_BUILD_VULKAN ON CACHE BOOL "" FORCE)
add_subdirectory("${RAWR_ROOT}/native/gainmap" "${CMAKE_CURRENT_BINARY_DIR}/gainmap")

# Film replay (optional, --film): adapter library only, no standalone
# validators/benchmarks. Spektra shaders are compiled here with the same
# flags the spektrafilm build uses.
set(SPEKTRA_FILM_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
add_subdirectory("${RAWR_ROOT}/native/spektrafilm" "${CMAKE_CURRENT_BINARY_DIR}/spektrafilm")

set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}")
set(RCD_SHADER_DIR "${RAWR_ROOT}/native/raw_demosaic/rcd/shaders")
set(DUAL_DIR "${RAWR_ROOT}/native/raw_demosaic/dual")
set(VNG_SHADER_DIR "${RAWR_ROOT}/native/raw_demosaic/vng4/shaders")
set(QUADFIX_SHADER_DIR "${RAWR_ROOT}/native/raw_demosaic/quadfix/shaders")
set(FCC_SHADER_DIR "${RAWR_ROOT}/native/false_color_correction/shaders")
set(TONEMAP_SHADER_DIR "${RAWR_ROOT}/native/tonemap_engine/shaders")
set(RAW_PREVIEW_SHADER_DIR "${RAWR_ROOT}/native/raw_preview/shaders")
set(RAW_HIGHLIGHT_SHADER_DIR "${RAWR_ROOT}/native/raw_highlight/shaders")
set(APP_CPP_DIR "${RAWR_ROOT}/app/src/main/cpp")

set(OFFLINE_SHADER_OUTPUTS)
function(rawr_compile_shader OUTPUT_NAME SOURCE)
  set(OUTPUT "${GEN}/${OUTPUT_NAME}.spv")
  set(EXTRA_SHADER_DEPENDS)
  if(SOURCE MATCHES "/rcd/shaders/")
    list(APPEND EXTRA_SHADER_DEPENDS ${RCD_SHADER_DIR}/rcd_common.glsl)
  endif()
  if(SOURCE STREQUAL "${TONEMAP_SHADER_DIR}/tonemap.comp")
    set(EXTRA_SHADER_DEPENDS ${TONEMAP_SHADER_DIR}/color/cst.glsl ${TONEMAP_SHADER_DIR}/color/gamut_matrices.inc ${TONEMAP_SHADER_DIR}/lut/lut3d.glsl)
  endif()
  add_custom_command(
    OUTPUT "${OUTPUT}"
    COMMAND "${GLSLC}" ${ARGN} "${SOURCE}" -o "${OUTPUT}"
    DEPENDS "${SOURCE}" ${EXTRA_SHADER_DEPENDS}
    VERBATIM)
  list(APPEND OFFLINE_SHADER_OUTPUTS "${OUTPUT}")
  set(OFFLINE_SHADER_OUTPUTS "${OFFLINE_SHADER_OUTPUTS}" PARENT_SCOPE)
endfunction()


# The same RCD workgroup variants embedded by the Android application.
rawr_compile_shader(rcd_direction.comp "${RCD_SHADER_DIR}/rcd_direction.comp"
  --target-env=vulkan1.1 -O -DRCD_LOCAL_X=4 -DRCD_LOCAL_Y=32 -I${RCD_SHADER_DIR})
rawr_compile_shader(rcd_green.comp "${RCD_SHADER_DIR}/rcd_green.comp"
  --target-env=vulkan1.1 -O -DRCD_LOCAL_X=4 -DRCD_LOCAL_Y=32 -I${RCD_SHADER_DIR})
rawr_compile_shader(rcd_diagonal.comp "${RCD_SHADER_DIR}/rcd_diagonal.comp"
  --target-env=vulkan1.1 -O -DRCD_LOCAL_X=32 -DRCD_LOCAL_Y=8 -I${RCD_SHADER_DIR})
rawr_compile_shader(rcd_green_sites.comp "${RCD_SHADER_DIR}/rcd_green_sites.comp"
  --target-env=vulkan1.1 -O -DRCD_LOCAL_X=8 -DRCD_LOCAL_Y=32 -I${RCD_SHADER_DIR})
rawr_compile_shader(rcd_export.comp "${RCD_SHADER_DIR}/rcd_export.comp"
  --target-env=vulkan1.1 -O -DRCD_LOCAL_X=4 -DRCD_LOCAL_Y=32 -I${RCD_SHADER_DIR})

foreach(NAME rcd_balance rcd_balance_reduce)
  rawr_compile_shader(${NAME}.comp "${RCD_SHADER_DIR}/${NAME}.comp" --target-env=vulkan1.1 -O -I${RCD_SHADER_DIR})
endforeach()

# The frozen VNG4 and dual-blend variants used in production stills.
rawr_compile_shader(vng4_linear.comp "${VNG_SHADER_DIR}/vng4_linear.comp"
  --target-env=vulkan1.1 -O -DVNG4_WGX=16 -DVNG4_WGY=16 -I${VNG_SHADER_DIR})
rawr_compile_shader(vng4_green.comp "${VNG_SHADER_DIR}/vng4_green.comp"
  --target-env=vulkan1.1 -O -DVNG4_WGX=16 -DVNG4_WGY=16 -I${VNG_SHADER_DIR})
rawr_compile_shader(vng4_export.comp "${VNG_SHADER_DIR}/vng4_export.comp"
  --target-env=vulkan1.1 -O -DVNG4_WGX=16 -DVNG4_WGY=16 -I${VNG_SHADER_DIR})
rawr_compile_shader(vng4_export_blend.comp "${VNG_SHADER_DIR}/vng4_export_blend.comp"
  --target-env=vulkan1.1 -O -DVNG4_WGX=16 -DVNG4_WGY=16 -I${VNG_SHADER_DIR})

foreach(NAME dual_luminance dual_mask dual_gauss_h dual_gauss_v dual_blend
             dual_auto_tiles dual_auto_select dual_auto_refine dual_auto_threshold)
  rawr_compile_shader(${NAME}.comp "${DUAL_DIR}/shaders/${NAME}.comp"
    --target-env=vulkan1.1 -O -fshader-stage=compute)
endforeach()

foreach(NAME quadfix_guide quadfix_guide_fast quadfix_energy quadfix_demod quadfix_blend)
  rawr_compile_shader(${NAME}.comp "${QUADFIX_SHADER_DIR}/${NAME}.comp"
    --target-env=vulkan1.1 -O -DQUADFIX_LOCAL_X=16 -DQUADFIX_LOCAL_Y=16 -I${QUADFIX_SHADER_DIR})
endforeach()
foreach(AXIS h v)
  set(AXIS_FLAGS)
  if(AXIS STREQUAL "v")
    set(AXIS_FLAGS -DQUADFIX_PASS_V=1)
  endif()
  rawr_compile_shader(quadfix_gauss_${AXIS}.comp "${QUADFIX_SHADER_DIR}/quadfix_gauss.comp"
    --target-env=vulkan1.1 -O -DQUADFIX_LOCAL_X=16 -DQUADFIX_LOCAL_Y=16 ${AXIS_FLAGS} -I${QUADFIX_SHADER_DIR})
  rawr_compile_shader(quadfix_mask_${AXIS}.comp "${QUADFIX_SHADER_DIR}/quadfix_mask.comp"
    --target-env=vulkan1.1 -O -DQUADFIX_LOCAL_X=16 -DQUADFIX_LOCAL_Y=16 ${AXIS_FLAGS} -I${QUADFIX_SHADER_DIR})
endforeach()


# App compilation intentionally leaves LOCAL_X/Y at shader defaults; preserve
# that exact behavior here while TonemapCreateInfo retains its app defaults.
rawr_compile_shader(tonemap.comp "${TONEMAP_SHADER_DIR}/tonemap.comp"
  -I${TONEMAP_SHADER_DIR})

# Production UltraHDR gain map stage.
set(GAINMAP_SHADER_DIR "${RAWR_ROOT}/native/gainmap/shaders")
rawr_compile_shader(gainmap.comp "${GAINMAP_SHADER_DIR}/gainmap.comp"
  --target-env=vulkan1.1 -O -fshader-stage=compute)
rawr_compile_shader(gainmap_blur.comp "${GAINMAP_SHADER_DIR}/gainmap_blur.comp"
  --target-env=vulkan1.1 -O -fshader-stage=compute)

# Spektra film shaders for the --film replay branch. Same compiler flags as
# the spektrafilm standalone build (-fshader-stage=compute, no target env).
set(SPEKTRA_SHADER_DIR "${RAWR_ROOT}/native/spektrafilm/shaders")
foreach(SPEKTRA_SHADER spektra_input.comp SpektraCopy.comp SpektraFormatConvert.comp SpektraFilmExposure.comp
                       SpektraCurveDevelop.comp SpektraPrintScan.comp SpektraHalation.comp SpektraDiffusion.comp
                       SpektraDir.comp SpektraScannerPost.comp SpektraGrain.comp spektra_output.comp
                       spektra_output_half.comp spektra_boost_milestone.comp spektra_glow_ratio.comp)
  set(SPEKTRA_SPV "${GEN}/${SPEKTRA_SHADER}.spv")
  add_custom_command(
    OUTPUT "${SPEKTRA_SPV}"
    COMMAND "${GLSLC}" -fshader-stage=compute "${SPEKTRA_SHADER_DIR}/${SPEKTRA_SHADER}" -o "${SPEKTRA_SPV}"
    DEPENDS "${SPEKTRA_SHADER_DIR}/${SPEKTRA_SHADER}"
    VERBATIM)
  list(APPEND OFFLINE_SHADER_OUTPUTS "${SPEKTRA_SPV}")
endforeach()

add_custom_target(rawr_offline_shaders DEPENDS ${OFFLINE_SHADER_OUTPUTS})

add_executable(rawr_dng_decode dng_decode.cpp "${APP_CPP_DIR}/renderer/DngSource.cpp")
target_include_directories(rawr_dng_decode PRIVATE "${APP_CPP_DIR}")
target_link_libraries(rawr_dng_decode PRIVATE rawr_tinydng)

add_executable(rawr_uhdr_mux "${RAWR_ROOT}/tools/uhdr_mux_cli/main.cpp"
  "${RAWR_ROOT}/native/gainmap/src/UltrahdrMux.cpp")
target_include_directories(rawr_uhdr_mux PRIVATE "${RAWR_ROOT}/native/gainmap/include")

add_executable(rawr_offline_pipeline
  main.cpp
  ${OFFLINE_SHADER_OUTPUTS})
add_dependencies(rawr_offline_pipeline rawr_offline_shaders)
target_include_directories(rawr_offline_pipeline PRIVATE
  "${APP_CPP_DIR}"
  "${GEN}"
  "${RAWR_ROOT}/native/gainmap/include"
  "${RAWR_ROOT}/native/vk_common/testing")
target_compile_definitions(rawr_offline_pipeline PRIVATE
  RAWR_OFFLINE_SHADER_DIR="${GEN}")
target_link_libraries(rawr_offline_pipeline PRIVATE
  rawr::post_demosaic
  rawr::vk_common
  raw_highlight::raw_highlight
  rawr::shader_common
  dual::dual_demosaic
  quadfix::quadfix
  fcc::false_color_correction
  TonemapEngine::Vulkan
  Gainmap::Compute
  spektrafilm::spektrafilm
  raw_denoise::raw_denoise
  Vulkan::Vulkan)
if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
  target_compile_options(rawr_offline_pipeline PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
