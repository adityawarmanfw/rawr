cmake_minimum_required(VERSION 3.22)
project(amaze_demosaic VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(ANDROID)
  set(VULKAN_LIB vulkan)
else()
  find_package(Vulkan REQUIRED)
  set(VULKAN_LIB Vulkan::Vulkan)
endif()

add_library(amaze STATIC src/Pipeline.cpp)
add_library(amaze::amaze ALIAS amaze)
target_include_directories(amaze PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_compile_features(amaze PUBLIC cxx_std_17)
target_compile_options(amaze PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-O3;-Wall;-Wextra;-Wpedantic;-Werror>)
target_link_libraries(amaze PUBLIC ${VULKAN_LIB})

# Host validation against librtprocess (see validation/README.md). Loads the
# committed naga SPIR-V plus the two GLSL glue passes compiled here.
option(AMAZE_BUILD_TESTS "Build standalone Vulkan validation harness" OFF)
if(AMAZE_BUILD_TESTS)
  find_program(GLSLC glslc REQUIRED)
  set(AMAZE_GLSL_OUT ${CMAKE_CURRENT_BINARY_DIR}/spv)
  file(MAKE_DIRECTORY ${AMAZE_GLSL_OUT})
  set(AMAZE_GLSL_SPV)
  foreach(NAME amaze_import amaze_export)
    add_custom_command(OUTPUT ${AMAZE_GLSL_OUT}/${NAME}.spv
      COMMAND ${GLSLC} --target-env=vulkan1.1 -O -I${CMAKE_CURRENT_SOURCE_DIR}/shaders
              ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${NAME}.comp -o ${AMAZE_GLSL_OUT}/${NAME}.spv
      DEPENDS shaders/${NAME}.comp shaders/amaze_tile.glsl VERBATIM)
    list(APPEND AMAZE_GLSL_SPV ${AMAZE_GLSL_OUT}/${NAME}.spv)
  endforeach()
  add_custom_target(amaze_glsl_spv DEPENDS ${AMAZE_GLSL_SPV})
  add_executable(amaze_validate validation/amaze_validate.cpp)
  add_dependencies(amaze_validate amaze_glsl_spv)
  target_link_libraries(amaze_validate PRIVATE amaze ${VULKAN_LIB})
  target_compile_options(amaze_validate PRIVATE $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-Wall;-Wextra;-Wpedantic;-Werror>)
  target_compile_definitions(amaze_validate PRIVATE
    AMAZE_WGSL_SPV_DIR="${CMAKE_CURRENT_SOURCE_DIR}/shaders/spv" AMAZE_GLSL_SPV_DIR="${AMAZE_GLSL_OUT}")
endif()
