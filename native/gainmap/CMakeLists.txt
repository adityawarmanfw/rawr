cmake_minimum_required(VERSION 3.20)
project(GainmapCompute VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(GAINMAP_WARNINGS_AS_ERRORS "Treat gainmap project warnings as errors" ON)
option(GAINMAP_BUILD_TESTS "Build gainmap validation executables" ON)

function(gainmap_enable_project_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
        if(GAINMAP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE /WX)
        endif()
    elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic)
        if(GAINMAP_WARNINGS_AS_ERRORS)
            target_compile_options(${target} PRIVATE -Werror)
        endif()
    endif()
endfunction()

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

add_library(gainmap_math INTERFACE)
target_include_directories(gainmap_math INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
add_library(Gainmap::Math ALIAS gainmap_math)

option(GAINMAP_BUILD_VULKAN "Build Vulkan gainmap backend" ON)
if(GAINMAP_BUILD_VULKAN)
    if(ANDROID)
        add_library(gainmap_compute STATIC src/GainmapCompute.cpp src/UltrahdrMux.cpp)
        target_include_directories(gainmap_compute PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
        target_link_libraries(gainmap_compute PUBLIC vulkan)
        gainmap_enable_project_warnings(gainmap_compute)
    else()
        find_package(Vulkan QUIET)
        if(Vulkan_FOUND)
            add_library(gainmap_compute STATIC src/GainmapCompute.cpp src/UltrahdrMux.cpp)
            target_include_directories(gainmap_compute PUBLIC
                $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
                $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>)
            target_link_libraries(gainmap_compute PUBLIC Vulkan::Vulkan)
            gainmap_enable_project_warnings(gainmap_compute)
        else()
            message(STATUS "Vulkan SDK not found: gainmap Vulkan backend will not be built")
        endif()
    endif()

    if(TARGET gainmap_compute)
        add_library(Gainmap::Compute ALIAS gainmap_compute)
        set_target_properties(gainmap_compute PROPERTIES OUTPUT_NAME gainmap_compute EXPORT_NAME Compute)
        if(GAINMAP_BUILD_TESTS)
            add_executable(gainmap_math_test tests/test_gainmap_math.cpp)
            target_link_libraries(gainmap_math_test PRIVATE gainmap_math gainmap_compute)
            gainmap_enable_project_warnings(gainmap_math_test)
            if(NOT ANDROID)
                add_executable(ultrahdr_mux_test tests/test_ultrahdr_mux.cpp)
                target_link_libraries(ultrahdr_mux_test PRIVATE gainmap_compute)
                gainmap_enable_project_warnings(ultrahdr_mux_test)
            endif()
            if(NOT ANDROID)
                add_executable(gainmap_vulkan_validation tests/gainmap_vulkan_validation.cpp)
                target_link_libraries(gainmap_vulkan_validation PRIVATE gainmap_compute)
                # --agx-tap checks the AgX tap against the tonemap engine's CPU reference;
                # --grade-tap di|logc3 checks the Grade tap against a CPU mirror.
                target_include_directories(gainmap_vulkan_validation PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tonemap_engine/include)
                target_sources(gainmap_vulkan_validation PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/../tonemap_engine/src/color/ColorSpace.cpp)
                gainmap_enable_project_warnings(gainmap_vulkan_validation)
                find_program(GLSLC_HOST glslc)
                if(GLSLC_HOST)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gainmap.spv
                        COMMAND ${GLSLC_HOST} --target-env=vulkan1.1 -O
                            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gainmap.comp -o ${CMAKE_CURRENT_BINARY_DIR}/gainmap.spv
                        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gainmap.comp
                                ${CMAKE_CURRENT_SOURCE_DIR}/../shader_common/shaders/adaptive_tone.glsl
                                ${CMAKE_CURRENT_SOURCE_DIR}/../tonemap_engine/shaders/agx.glsl
                        VERBATIM)
                    add_custom_command(OUTPUT ${CMAKE_CURRENT_BINARY_DIR}/gainmap_blur.spv
                        COMMAND ${GLSLC_HOST} --target-env=vulkan1.1 -O
                            ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gainmap_blur.comp -o ${CMAKE_CURRENT_BINARY_DIR}/gainmap_blur.spv
                        DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/shaders/gainmap_blur.comp
                        VERBATIM)
                    add_custom_target(gainmap_test_spv DEPENDS ${CMAKE_CURRENT_BINARY_DIR}/gainmap.spv
                                                              ${CMAKE_CURRENT_BINARY_DIR}/gainmap_blur.spv)
                    add_dependencies(gainmap_vulkan_validation gainmap_test_spv)
                endif()
            endif()
        endif()
    endif()
endif()

install(DIRECTORY include/ DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
