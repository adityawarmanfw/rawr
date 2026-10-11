cmake_minimum_required(VERSION 3.22)
project(spektrafilm VERSION 0.1.0 LANGUAGES CXX)

option(SPEKTRA_FILM_BUILD_STANDALONE "Build standalone shader artifacts, validators, and benchmarks" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(ANDROID)
    find_library(VULKAN_LIB vulkan REQUIRED)
else()
    find_package(Vulkan REQUIRED)
    set(VULKAN_LIB Vulkan::Vulkan)
endif()

# Core adapter library (record-only, no device ownership).
add_library(spektrafilm STATIC
    src/SpektraFilm.cpp
    src/SpektraTables.cpp
    generated/SpektraGeneratedProfileCurves.cpp
)
add_library(spektrafilm::spektrafilm ALIAS spektrafilm)

target_include_directories(spektrafilm PUBLIC include)
target_include_directories(spektrafilm PRIVATE
    include/spektrafilm
    generated
    src
)
target_link_libraries(spektrafilm PUBLIC ${VULKAN_LIB})
if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    target_compile_options(spektrafilm PRIVATE -Wall -Wextra -Wpedantic -Werror -O3)
endif()

# Vendored upstream reference renderer (parity baseline + harness).
# Deliberately NOT warning-clean: third-party code, see NOTICE.md.
# Standalone-only: the app links libspektrafilm alone.
if(SPEKTRA_FILM_BUILD_STANDALONE)
add_library(spektra_ref STATIC
    reference/SpektraVulkanRenderer.cpp
)
add_library(spektrafilm::ref ALIAS spektra_ref)

target_include_directories(spektra_ref PRIVATE
    reference
    include/spektrafilm
    generated
)
target_link_libraries(spektra_ref PUBLIC ${VULKAN_LIB} spektrafilm::spektrafilm)
endif()

if(SPEKTRA_FILM_BUILD_STANDALONE)
    find_program(GLSLANG_VALIDATOR glslangValidator)
    find_program(GLSLC glslc)
    if(NOT GLSLANG_VALIDATOR AND NOT GLSLC)
        message(FATAL_ERROR "No GLSL-to-SPIR-V compiler found (glslangValidator or glslc)")
    endif()

    # All upstream shaders (reference parity needs every .spv present) plus
    # the two adapter-owned shaders. Debug-only shaders
    # (spektra_output_vis, spektra_passthrough) are compiled manually.
    set(SPEKTRA_FILM_SHADERS
        shaders/spektra_input.comp
        shaders/SpektraCopy.comp
        shaders/SpektraFormatConvert.comp
        shaders/SpektraFilmExposure.comp
        shaders/SpektraCurveDevelop.comp
        shaders/SpektraPrintScan.comp
        shaders/SpektraHalation.comp
        shaders/SpektraDiffusion.comp
        shaders/SpektraDir.comp
        shaders/SpektraScannerPost.comp
        shaders/SpektraGrain.comp
        shaders/spektra_output.comp
        shaders/spektra_output_half.comp
        shaders/spektra_boost_milestone.comp
        shaders/spektra_glow_ratio.comp
    )
    set(SPEKTRA_FILM_SPV)
    foreach(SHADER ${SPEKTRA_FILM_SHADERS})
        get_filename_component(SHADER_NAME ${SHADER} NAME)
        set(SPV ${CMAKE_BINARY_DIR}/${SHADER_NAME}.spv)
        list(APPEND SPEKTRA_FILM_SPV ${SPV})
        if(GLSLANG_VALIDATOR)
            add_custom_command(
                OUTPUT ${SPV}
                COMMAND ${GLSLANG_VALIDATOR} -V ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPV}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} ${CMAKE_CURRENT_SOURCE_DIR}/../shader_common/shaders/adaptive_tone.glsl
                VERBATIM
            )
        else()
            add_custom_command(
                OUTPUT ${SPV}
                COMMAND ${GLSLC} -fshader-stage=compute ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} -o ${SPV}
                DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/${SHADER} ${CMAKE_CURRENT_SOURCE_DIR}/../shader_common/shaders/adaptive_tone.glsl
                VERBATIM
            )
        endif()
    endforeach()
    add_custom_target(spektrafilm_shaders ALL DEPENDS ${SPEKTRA_FILM_SPV})

    add_executable(spektrafilm_validation
        validation/spektrafilm_validation.cpp
    )
    target_include_directories(spektrafilm_validation PRIVATE tools reference include/spektrafilm)
    target_link_libraries(spektrafilm_validation PRIVATE spektrafilm::spektrafilm spektrafilm::ref)
    if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
        target_compile_options(spektrafilm_validation PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_dependencies(spektrafilm_validation spektrafilm_shaders)

    add_executable(spektrafilm_bench
        bench/spektrafilm_bench.cpp
    )
    target_include_directories(spektrafilm_bench PRIVATE tools)
    target_link_libraries(spektrafilm_bench PRIVATE spektrafilm::spektrafilm)
    if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
        target_compile_options(spektrafilm_bench PRIVATE -Wall -Wextra -Wpedantic -Werror)
    endif()
    add_dependencies(spektrafilm_bench spektrafilm_shaders)

    add_executable(spektra_ref_harness
        reference/SpektraVulkanCopyHarness.cpp
    )
    target_include_directories(spektra_ref_harness PRIVATE reference include/spektrafilm)
    target_link_libraries(spektra_ref_harness PRIVATE spektrafilm::ref spektrafilm::spektrafilm)
endif()
