cmake_minimum_required(VERSION 3.22)
project(rawr_multiframe_moltenvk_replay LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

get_filename_component(RAWR_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)

# The multiframe stack builds exactly as in the app, except that sharpness
# links only its host reference.
set(RAW_SHARPNESS_BUILD_VULKAN OFF CACHE BOOL "" FORCE)
set(RAW_SHARPNESS_BUILD_TESTS OFF CACHE BOOL "" FORCE)
add_subdirectory("${RAWR_ROOT}/native/multiframe" "${CMAKE_CURRENT_BINARY_DIR}/multiframe")
# DNG burst input decodes through the app's production DngSource.
add_subdirectory("${RAWR_ROOT}/native/tinydng" "${CMAKE_CURRENT_BINARY_DIR}/tinydng")

set(GEN "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${GEN}")
set(EMBED "${RAWR_ROOT}/native/vk_common/cmake/EmbedBinary.cmake")
set(ZSL_SPV "${GEN}/raw_zsl_codec_decode.spv")
set(ZSL_HDR "${GEN}/raw_zsl_codec_decode.h")
add_custom_command(OUTPUT "${ZSL_SPV}" "${ZSL_HDR}"
  COMMAND "${GLSLC}" --target-env=vulkan1.1 -O
    "${RAWR_ROOT}/native/zsl_codec/raw_zsl_codec_decode.comp" -o "${ZSL_SPV}"
  COMMAND "${CMAKE_COMMAND}" -DINPUT=${ZSL_SPV} -DOUTPUT=${ZSL_HDR}
    -DSYMBOL=raw_zsl_codec_decode_spv -P "${EMBED}"
  DEPENDS "${RAWR_ROOT}/native/zsl_codec/raw_zsl_codec_decode.comp" "${EMBED}" VERBATIM)
add_custom_target(rawr_multiframe_replay_shaders DEPENDS "${ZSL_SPV}" "${ZSL_HDR}")

add_executable(rawr_multiframe_moltenvk_replay
  main.cpp
  "${RAWR_ROOT}/native/zsl_container/src/ZslContainer.cpp"
  "${RAWR_ROOT}/native/zsl_codec/src/ZslDecoder.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/ColorMath.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/ColorCalibration.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/color/DcpProfile.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/renderer/DngSource.cpp"
  "${RAWR_ROOT}/app/src/main/cpp/renderer/SourceCaptureExif.cpp"
  "${ZSL_HDR}")
add_dependencies(rawr_multiframe_moltenvk_replay rawr_multiframe_replay_shaders)
target_include_directories(rawr_multiframe_moltenvk_replay PRIVATE
  "${GEN}"
  "${RAWR_ROOT}/app/src/main/cpp"
  "${RAWR_ROOT}/native/vk_common/testing"
  "${RAWR_ROOT}/native/zsl_container/include")
target_link_libraries(rawr_multiframe_moltenvk_replay PRIVATE raw_gpu_pipeline raw_multiframe_output raw_sharpness::reference rawr_tinydng Vulkan::Vulkan)
if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
  target_compile_options(rawr_multiframe_moltenvk_replay PRIVATE -O3 -Wall -Wextra -Wpedantic)
endif()
