cmake_minimum_required(VERSION 3.22)
project(raw_multiframe_output LANGUAGES CXX)

# Multiframe output adapter: projects the merged RGB back to CFA for DNG and
# prepares the JPEG path's RGB and clip evidence.
if(ANDROID)
  find_library(VULKAN_LIB vulkan REQUIRED)
else()
  find_package(Vulkan REQUIRED)
  set(VULKAN_LIB Vulkan::Vulkan)
endif()
if(NOT GLSLC)
  find_program(GLSLC glslc REQUIRED)
endif()
if(NOT COMMAND embed_spirv)
  include(${CMAKE_CURRENT_LIST_DIR}/../../vk_common/cmake/EmbedSpirv.cmake)
endif()

set(MF_OUTPUT_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${MF_OUTPUT_GEN})
set(SHADER_COMMON_DIR ${CMAKE_CURRENT_LIST_DIR}/../../shader_common/shaders)
embed_spirv(multiframe_cfa_project ${CMAKE_CURRENT_LIST_DIR}/shaders/project_cfa_u16.comp multiframe_cfa_project_spv
  OUTPUT_DIR ${MF_OUTPUT_GEN})
embed_spirv(multiframe_cfa_project_f32 ${CMAKE_CURRENT_LIST_DIR}/shaders/project_cfa_u16.comp
  multiframe_cfa_project_f32_spv OUTPUT_DIR ${MF_OUTPUT_GEN} FLAGS -DRAWR_SOURCE_R32F)
embed_spirv(multiframe_rgb_prepare ${CMAKE_CURRENT_LIST_DIR}/shaders/prepare_rgb.comp multiframe_rgb_prepare_spv
  OUTPUT_DIR ${MF_OUTPUT_GEN} FLAGS -I${SHADER_COMMON_DIR} DEPENDS ${SHADER_COMMON_DIR}/cfa_common.glsl)

embed_spirv(multiframe_packed_cfa ${CMAKE_CURRENT_LIST_DIR}/shaders/prepare_packed_cfa.comp multiframe_packed_cfa_spv
  OUTPUT_DIR ${MF_OUTPUT_GEN} FLAGS -I${SHADER_COMMON_DIR}
  DEPENDS ${SHADER_COMMON_DIR}/cfa_common.glsl ${SHADER_COMMON_DIR}/lens_shading.glsl)

add_library(raw_multiframe_output STATIC
  src/MultiframeOutputAdapter.cpp
  ${multiframe_cfa_project_HDR} ${multiframe_cfa_project_f32_HDR} ${multiframe_rgb_prepare_HDR} ${multiframe_packed_cfa_HDR})
target_include_directories(raw_multiframe_output PUBLIC include PRIVATE ${MF_OUTPUT_GEN})
target_link_libraries(raw_multiframe_output PUBLIC ${VULKAN_LIB})
target_compile_features(raw_multiframe_output PUBLIC cxx_std_17)
target_compile_options(raw_multiframe_output PRIVATE -Wall -Wextra -Wpedantic -Werror)
