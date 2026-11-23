cmake_minimum_required(VERSION 3.22)
project(rawr_still_perf_bench LANGUAGES C CXX)
# Host (MoltenVK) still-pipeline benchmark: the production native libraries in
# app order (clip pack + LSC, AMaZE, post-demosaic: WB/HL/distortion, defringe,
# FCC, gain map) plus the CPU JPEG/UltraHDR encode, with per-stage GPU time,
# wall time and device-memory accounting. See README.md.
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../.." ABSOLUTE)
set(N ${ROOT}/native)
set(APP ${ROOT}/app/src/main/cpp)
if(ANDROID)
  find_library(VULKAN_LIB vulkan REQUIRED)
else()
  find_package(Vulkan REQUIRED)
  set(VULKAN_LIB Vulkan::Vulkan)
endif()
find_package(Threads REQUIRED)
find_program(GLSLC glslc REQUIRED)
include(${N}/vk_common/cmake/EmbedSpirv.cmake)

add_subdirectory(${N}/post_demosaic post_demosaic)
set(AMAZE_BUILD_TESTS ON CACHE BOOL "" FORCE)
add_subdirectory(${N}/raw_demosaic/amaze amaze)
set(GAINMAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory(${N}/gainmap gainmap)

set(GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${GEN})
set(SC ${N}/shader_common/shaders)
embed_spirv(sensor_clip_pack ${N}/raw_demosaic/common/shaders/sensor_clip_pack.comp sensor_clip_pack_spv
  OUTPUT_DIR ${GEN} HEADERS_VAR BENCH_HEADERS FLAGS -I${SC} DEPENDS ${SC}/lens_shading.glsl ${SC}/cfa_common.glsl)
embed_spirv(gainmap ${N}/gainmap/shaders/gainmap.comp gainmap_spv OUTPUT_DIR ${GEN} HEADERS_VAR BENCH_HEADERS
  FLAGS --target-env=vulkan1.1 -O -I${SC} -I${N}/tonemap_engine/shaders)
embed_spirv(gainmap_blur ${N}/gainmap/shaders/gainmap_blur.comp gainmap_blur_spv OUTPUT_DIR ${GEN}
  HEADERS_VAR BENCH_HEADERS FLAGS --target-env=vulkan1.1 -O)

# Vendored libjpeg-turbo as a nested project (upstream rejects add_subdirectory).
include(ExternalProject)
set(JT_BIN ${CMAKE_CURRENT_BINARY_DIR}/libjpeg-turbo-build)
ExternalProject_Add(libjpeg_turbo_external
  SOURCE_DIR ${N}/libjpeg-turbo
  BINARY_DIR ${JT_BIN}
  CMAKE_ARGS -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE=${CMAKE_TOOLCHAIN_FILE} -DANDROID_ABI=${ANDROID_ABI}
             -DANDROID_PLATFORM=${ANDROID_PLATFORM} -DCMAKE_MAKE_PROGRAM=${CMAKE_MAKE_PROGRAM} -DENABLE_SHARED=OFF -DENABLE_STATIC=ON -DWITH_TURBOJPEG=ON
             -DWITH_TOOLS=OFF -DWITH_TESTS=OFF
  BUILD_COMMAND ${CMAKE_COMMAND} --build <BINARY_DIR> --target turbojpeg-static
  INSTALL_COMMAND ""
  BUILD_BYPRODUCTS ${JT_BIN}/libturbojpeg.a)
add_library(bench_turbojpeg STATIC IMPORTED)
set_target_properties(bench_turbojpeg PROPERTIES IMPORTED_LOCATION ${JT_BIN}/libturbojpeg.a)
add_dependencies(bench_turbojpeg libjpeg_turbo_external)

add_executable(still_perf_bench main.cpp ${APP}/encoding/jpeg/StripJpegEncoder.cpp ${BENCH_HEADERS})
add_dependencies(still_perf_bench amaze_glsl_spv libjpeg_turbo_external)
target_include_directories(still_perf_bench PRIVATE ${GEN} ${APP} ${N}/vk_common/testing ${N}/libjpeg-turbo/src
  ${JT_BIN})
target_link_libraries(still_perf_bench PRIVATE rawr::post_demosaic amaze Gainmap::Compute bench_turbojpeg
  Threads::Threads ${VULKAN_LIB})
# Baseline builds of the libraries (before releaseScratch existed) set this OFF.
option(BENCH_RELEASE_SCRATCH "Call PostDemosaicProcessor::releaseScratch" ON)
if(BENCH_RELEASE_SCRATCH)
  target_compile_definitions(still_perf_bench PRIVATE BENCH_RELEASE_SCRATCH=1)
endif()
target_compile_definitions(still_perf_bench PRIVATE
  AMAZE_WGSL_SPV_DIR="${N}/raw_demosaic/amaze/shaders/spv"
  AMAZE_GLSL_SPV_DIR="${CMAKE_CURRENT_BINARY_DIR}/amaze/spv")
