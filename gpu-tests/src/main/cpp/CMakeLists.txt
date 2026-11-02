cmake_minimum_required(VERSION 3.24)
project(rawr_headless_tests LANGUAGES CXX)
get_filename_component(ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../.." ABSOLUTE)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(RAW_PREVIEW_BUILD_STANDALONE OFF CACHE BOOL "" FORCE)
set(TONEMAP_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${ROOT}/native/raw_preview" raw_preview)
add_subdirectory("${ROOT}/native/tonemap_engine" tonemap)
find_program(GLSLC glslc HINTS "${ANDROID_NDK}/shader-tools/darwin-x86_64" "${ANDROID_NDK}/shader-tools/linux-x86_64" REQUIRED)
include("${ROOT}/native/vk_common/cmake/EmbedSpirv.cmake")
set(EMBED_SPIRV_OUTPUT_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${EMBED_SPIRV_OUTPUT_DIR}")
set(HL "${ROOT}/native/raw_highlight/shaders")
set(COMMON "${ROOT}/native/shader_common/shaders")
file(GLOB SHADER_DEPS "${HL}/*.glsl" "${COMMON}/*.glsl" "${ROOT}/native/tonemap_engine/shaders/color/*" "${ROOT}/native/tonemap_engine/shaders/lut/*.glsl")
foreach(NAME raw_preview raw_preview_cfa_state)
  embed_spirv(${NAME} "${ROOT}/native/raw_preview/shaders/${NAME}.comp" ${NAME}_bytes
    FLAGS -I${HL} -I${COMMON} DEPENDS ${SHADER_DEPS} HEADERS_VAR HEADERS)
endforeach()
embed_spirv(raw_preview_cfa_optimized "${ROOT}/native/raw_preview/shaders/raw_preview_cfa_state.comp" raw_preview_cfa_optimized_bytes
  FLAGS -O -I${HL} -I${COMMON} DEPENDS ${SHADER_DEPS} HEADERS_VAR HEADERS)
embed_spirv(raw_preview_cfa_full_halo "${ROOT}/native/raw_preview/shaders/raw_preview_cfa_state.comp" raw_preview_cfa_full_halo_bytes
  FLAGS -DRAWR_PREVIEW_CLASSIFY_HALO=1 -I${HL} -I${COMMON} DEPENDS ${SHADER_DEPS} HEADERS_VAR HEADERS)
foreach(NAME highlight_guide_seed highlight_guide_propagate highlight_guide_smooth highlight_apply_inplace coloropp coloropp_tone)
  embed_spirv(${NAME} "${HL}/${NAME}.comp" ${NAME}_bytes
    FLAGS -DRAWR_HL_SCALE=1 -DRAWR_COLOROPP_POST_WB=1 -I${HL} -I${COMMON}
    DEPENDS ${SHADER_DEPS} HEADERS_VAR HEADERS)
endforeach()
embed_spirv(highlight_guide_seed_reference "${HL}/highlight_guide_seed.comp" highlight_guide_seed_reference_bytes
  FLAGS -DRAWR_HL_SCALE=1 -DRAWR_HL_SEED_EARLY_REJECT=0 -DRAWR_HL_SEED_GROUP_SIZE=16 -I${HL} -I${COMMON}
  DEPENDS ${SHADER_DEPS} HEADERS_VAR HEADERS)
embed_spirv(highlight_apply_reference "${HL}/highlight_apply_inplace.comp" highlight_apply_reference_bytes
  FLAGS -DRAWR_HL_SCALE=1 -DRAWR_HL_EARLY_RGB_READ=1 -I${HL} -I${COMMON}
  DEPENDS ${SHADER_DEPS} HEADERS_VAR HEADERS)
embed_spirv(tonemap "${ROOT}/native/tonemap_engine/shaders/tonemap.comp" tonemap_bytes
  FLAGS -DLOCAL_X=16 -DLOCAL_Y=8 -I${ROOT}/native/tonemap_engine/shaders -I${COMMON}
  DEPENDS ${SHADER_DEPS} "${ROOT}/native/tonemap_engine/shaders/agx.glsl" HEADERS_VAR HEADERS)
set(RGB "${ROOT}/native/raw_preview/validation/raw_preview_validation.cpp")
embed_spirv(tonemap_reference "${ROOT}/native/tonemap_engine/shaders/tonemap.comp" tonemap_reference_bytes
  FLAGS -DLOCAL_X=16 -DLOCAL_Y=8 -DRAWR_TONEMAP_COMBINED_CST=0 -I${ROOT}/native/tonemap_engine/shaders -I${COMMON}
  DEPENDS ${SHADER_DEPS} "${ROOT}/native/tonemap_engine/shaders/agx.glsl" HEADERS_VAR HEADERS)
set(CFA "${ROOT}/native/raw_preview/validation/cfa_state_validation.cpp")
set(BENCH "${ROOT}/native/raw_preview/bench/cfa_state_benchmark.cpp")
set(TONE "${ROOT}/native/tonemap_engine/tests/vulkan_runner.cpp")
set(READBACK "${ROOT}/tools/phone-performance/readback_check.cpp")
set_source_files_properties(${RGB} PROPERTIES COMPILE_DEFINITIONS "main=rawr_rgb_validation")
set_source_files_properties(${CFA} PROPERTIES COMPILE_DEFINITIONS "main=rawr_cfa_validation")
set_source_files_properties(${BENCH} PROPERTIES COMPILE_DEFINITIONS "main=rawr_preview_benchmark")
set_source_files_properties(${TONE} PROPERTIES COMPILE_DEFINITIONS "main=rawr_tonemap_validation")
set_source_files_properties(${READBACK} PROPERTIES COMPILE_DEFINITIONS "main=rawr_readback_validation")
add_library(rawr_gpu_tests SHARED HeadlessJni.cpp ${RGB} ${CFA} ${BENCH} ${TONE} ${READBACK} ${HEADERS})
target_include_directories(rawr_gpu_tests PRIVATE "${EMBED_SPIRV_OUTPUT_DIR}" "${ROOT}/app/src/main/cpp" "${ROOT}/native/vk_common/testing")
target_compile_options(rawr_gpu_tests PRIVATE -O2 -Wall -Wextra -Werror)
target_link_libraries(rawr_gpu_tests PRIVATE raw_preview tonemap_engine vulkan log)
