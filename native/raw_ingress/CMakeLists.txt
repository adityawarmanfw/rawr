cmake_minimum_required(VERSION 3.22)
project(raw_ingress LANGUAGES CXX)

# Camera RAW ingress on the GPU: RAW10 (MIPI packed) -> R16 unpack straight
# from the imported camera buffer.
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
  include(${CMAKE_CURRENT_LIST_DIR}/../vk_common/cmake/EmbedSpirv.cmake)
endif()
set(RAWR_NATIVE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
if(NOT TARGET rawr::vk_common)
  add_subdirectory(${RAWR_NATIVE_DIR}/vk_common ${CMAKE_CURRENT_BINARY_DIR}/vk_common)
endif()

set(RAW_INGRESS_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${RAW_INGRESS_GEN})
embed_spirv(raw10_unpack ${CMAKE_CURRENT_LIST_DIR}/shaders/raw10_unpack.comp raw10_unpack_spv
  OUTPUT_DIR ${RAW_INGRESS_GEN} FLAGS --target-env=vulkan1.1 -O)

add_library(raw_ingress STATIC src/Raw10Unpacker.cpp ${raw10_unpack_HDR})
add_library(rawr::raw_ingress ALIAS raw_ingress)
target_include_directories(raw_ingress PUBLIC include PRIVATE ${RAW_INGRESS_GEN})
target_link_libraries(raw_ingress PUBLIC rawr::vk_common ${VULKAN_LIB})
target_compile_features(raw_ingress PUBLIC cxx_std_17)
target_compile_options(raw_ingress PRIVATE -Wall -Wextra -Werror)
