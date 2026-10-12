cmake_minimum_required(VERSION 3.22)
project(image_scopes VERSION 0.1.0 LANGUAGES CXX)

option(IMAGE_SCOPES_BUILD_VULKAN "Build Vulkan backend" ON)
option(IMAGE_SCOPES_BUILD_TESTS "Build deterministic CPU/ABI validation" ON)
option(IMAGE_SCOPES_BUILD_BENCHMARK "Build CPU benchmark" ON)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

function(image_scopes_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX)
  else()
    target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -Werror)
  endif()
endfunction()

# The CPU reference implementation is validation/benchmark infrastructure, not a
# production dependency of the Vulkan library. Keep it out of the default Android
# app graph when both standalone validation and benchmarks are disabled.
if(IMAGE_SCOPES_BUILD_TESTS OR IMAGE_SCOPES_BUILD_BENCHMARK)
  add_library(image_scopes_reference STATIC reference/image_scopes_cpu.cpp)
  target_include_directories(image_scopes_reference PUBLIC include reference)
  target_compile_definitions(image_scopes_reference PRIVATE IMAGE_SCOPES_HAS_VULKAN=0)
  image_scopes_warnings(image_scopes_reference)
endif()

if(IMAGE_SCOPES_BUILD_VULKAN)
  if(ANDROID)
    find_library(IMAGE_SCOPES_VULKAN_LIB vulkan REQUIRED)
    set(IMAGE_SCOPES_VULKAN_FOUND TRUE)
  else()
    find_package(Vulkan QUIET)
    if(Vulkan_FOUND)
      set(IMAGE_SCOPES_VULKAN_LIB Vulkan::Vulkan)
      set(IMAGE_SCOPES_VULKAN_FOUND TRUE)
    endif()
  endif()

  if(IMAGE_SCOPES_VULKAN_FOUND)
    find_program(GLSLANG_VALIDATOR glslangValidator)
    find_program(GLSLC glslc)
    if(NOT GLSLANG_VALIDATOR AND NOT GLSLC)
      message(FATAL_ERROR "image_scopes: no GLSL-to-SPIR-V compiler found")
    endif()
    find_program(UV_EXECUTABLE uv REQUIRED)
    set(GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
    file(MAKE_DIRECTORY ${GEN})
    set(SHADERS exposure_stats_measure exposure_stats_reduce scene_exposure_measure scene_exposure_reduce waveform_measure vectorscope_measure vectorscope_reduce render_waveform render_vectorscope render_raw_waveform)
    set(SPVS)
    foreach(shader IN LISTS SHADERS)
      set(src ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${shader}.comp)
      set(spv ${GEN}/${shader}.spv)
      if(GLSLANG_VALIDATOR)
        add_custom_command(OUTPUT ${spv}
          COMMAND ${GLSLANG_VALIDATOR} -V --target-env vulkan1.1 -I${CMAKE_CURRENT_SOURCE_DIR}/shaders ${src} -o ${spv}
          DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shared/common.glsl VERBATIM)
      else()
        add_custom_command(OUTPUT ${spv}
          COMMAND ${GLSLC} --target-env=vulkan1.1 -fshader-stage=compute -I${CMAKE_CURRENT_SOURCE_DIR}/shaders ${src} -o ${spv}
          DEPENDS ${src} ${CMAKE_CURRENT_SOURCE_DIR}/shaders/shared/common.glsl VERBATIM)
      endif()
      list(APPEND SPVS ${spv})
    endforeach()
    add_custom_command(OUTPUT ${GEN}/image_scopes_shaders_spv.hpp
      COMMAND "${UV_EXECUTABLE}" run --no-project --cache-dir "${CMAKE_CURRENT_SOURCE_DIR}/../../tmp/uv-cache" ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py ${GEN}/image_scopes_shaders_spv.hpp ${SPVS}
      DEPENDS ${SPVS} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_spirv.py VERBATIM)
    add_custom_target(image_scopes_shaders DEPENDS ${GEN}/image_scopes_shaders_spv.hpp)

    add_library(image_scopes STATIC src/image_scopes_vulkan.cpp)
    add_library(image_scopes::image_scopes ALIAS image_scopes)
    add_dependencies(image_scopes image_scopes_shaders)
    target_include_directories(image_scopes PUBLIC include PRIVATE ${GEN})
    target_compile_definitions(image_scopes PUBLIC IMAGE_SCOPES_HAS_VULKAN=1)
    target_link_libraries(image_scopes PUBLIC ${IMAGE_SCOPES_VULKAN_LIB})
    image_scopes_warnings(image_scopes)

    if(IMAGE_SCOPES_BUILD_TESTS)
      add_executable(image_scopes_vulkan_validation
          tests/vulkan_test_context.cpp tests/vulkan_validation.cpp)
      target_include_directories(image_scopes_vulkan_validation PRIVATE include reference tests src)
      target_link_libraries(image_scopes_vulkan_validation PRIVATE image_scopes image_scopes_reference ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_vulkan_validation)
      add_test(NAME image_scopes_vulkan_validation COMMAND image_scopes_vulkan_validation)
      add_executable(image_scopes_scene_exposure_validation
          tests/vulkan_test_context.cpp tests/scene_exposure_validation.cpp)
      target_include_directories(image_scopes_scene_exposure_validation PRIVATE include reference tests src)
      target_link_libraries(image_scopes_scene_exposure_validation PRIVATE image_scopes image_scopes_reference ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_scene_exposure_validation)
      add_test(NAME image_scopes_scene_exposure_validation COMMAND image_scopes_scene_exposure_validation)
      add_executable(image_scopes_production_geometry_validation
          tests/vulkan_test_context.cpp tests/production_geometry_validation.cpp)
      target_include_directories(image_scopes_production_geometry_validation PRIVATE
          include reference tests src)
      target_link_libraries(image_scopes_production_geometry_validation PRIVATE image_scopes image_scopes_reference ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_production_geometry_validation)

      add_executable(image_scopes_vulkan_artifacts
          tests/vulkan_test_context.cpp tests/vulkan_artifacts.cpp)
      target_include_directories(image_scopes_vulkan_artifacts PRIVATE include tests src)
      target_link_libraries(image_scopes_vulkan_artifacts PRIVATE image_scopes ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_vulkan_artifacts)

      add_executable(image_scopes_vectorscope_photo_artifacts
          tests/vulkan_test_context.cpp tests/vectorscope_photo_artifacts.cpp)
      target_include_directories(image_scopes_vectorscope_photo_artifacts PRIVATE include tests src)
      target_link_libraries(image_scopes_vectorscope_photo_artifacts PRIVATE image_scopes ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_vectorscope_photo_artifacts)

      add_executable(image_scopes_waveform_photo_artifacts
          tests/vulkan_test_context.cpp tests/waveform_photo_artifacts.cpp)
      target_include_directories(image_scopes_waveform_photo_artifacts PRIVATE include tests src)
      target_link_libraries(image_scopes_waveform_photo_artifacts PRIVATE image_scopes ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_waveform_photo_artifacts)

      add_executable(image_scopes_vulkan_stability
          tests/vulkan_test_context.cpp tests/vulkan_stability.cpp)
      target_include_directories(image_scopes_vulkan_stability PRIVATE include tests src)
      target_link_libraries(image_scopes_vulkan_stability PRIVATE image_scopes ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_vulkan_stability)
    endif()

    if(IMAGE_SCOPES_BUILD_BENCHMARK)
      add_executable(image_scopes_vulkan_benchmark
          tests/vulkan_test_context.cpp benchmark/vulkan_benchmark.cpp)
      target_include_directories(image_scopes_vulkan_benchmark PRIVATE include tests src)
      target_link_libraries(image_scopes_vulkan_benchmark PRIVATE image_scopes ${IMAGE_SCOPES_VULKAN_LIB})
      image_scopes_warnings(image_scopes_vulkan_benchmark)
    endif()
  else()
    message(WARNING "image_scopes: Vulkan not found; Vulkan backend skipped")
  endif()
endif()

if(IMAGE_SCOPES_BUILD_TESTS)
  enable_testing()
  add_executable(image_scopes_validation tests/cpu_validation.cpp)
  target_link_libraries(image_scopes_validation PRIVATE image_scopes_reference)
  image_scopes_warnings(image_scopes_validation)
  add_test(NAME image_scopes_validation COMMAND image_scopes_validation)

  add_executable(image_scopes_abi_validation tests/abi_validation.cpp)
  target_include_directories(image_scopes_abi_validation PRIVATE include)
  target_compile_definitions(image_scopes_abi_validation PRIVATE IMAGE_SCOPES_HAS_VULKAN=0)
  image_scopes_warnings(image_scopes_abi_validation)
  add_test(NAME image_scopes_abi_validation COMMAND image_scopes_abi_validation)

  add_executable(image_scopes_sampling_quality tests/sampling_quality.cpp)
  target_link_libraries(image_scopes_sampling_quality PRIVATE image_scopes_reference)
  image_scopes_warnings(image_scopes_sampling_quality)
endif()

if(IMAGE_SCOPES_BUILD_BENCHMARK)
  add_executable(image_scopes_cpu_benchmark benchmark/cpu_benchmark.cpp)
  target_link_libraries(image_scopes_cpu_benchmark PRIVATE image_scopes_reference)
  image_scopes_warnings(image_scopes_cpu_benchmark)
endif()
