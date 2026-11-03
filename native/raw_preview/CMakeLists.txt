cmake_minimum_required(VERSION 3.22)
project(raw_preview VERSION 1.2.0 LANGUAGES CXX)

option(RAW_PREVIEW_BUILD_STANDALONE "Build standalone shader artifacts, validators, and benchmarks" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_COMPILE_WARNING_AS_ERROR ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "AppleClang|Clang|GNU")
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

if(ANDROID)
    find_library(VULKAN_LIB vulkan REQUIRED)
else()
    find_package(Vulkan REQUIRED)
    set(VULKAN_LIB Vulkan::Vulkan)
endif()

if(NOT TARGET raw_highlight::raw_highlight)
    add_subdirectory(${CMAKE_CURRENT_LIST_DIR}/../raw_highlight ${CMAKE_CURRENT_BINARY_DIR}/raw_highlight)
endif()

add_library(raw_preview STATIC
    src/RawPreview.cpp
)
add_library(raw_preview::raw_preview ALIAS raw_preview)

target_include_directories(raw_preview PUBLIC include)
target_link_libraries(raw_preview PUBLIC ${VULKAN_LIB} rawr::vk_common PRIVATE raw_highlight::raw_highlight)
target_compile_options(raw_preview PRIVATE
    $<$<CXX_COMPILER_ID:AppleClang,Clang,GNU>:-O3>
)

if(RAW_PREVIEW_BUILD_STANDALONE)
    find_program(GLSLANG_VALIDATOR glslangValidator)
    find_program(GLSLC glslc)

    set(SHADER_SRC ${CMAKE_SOURCE_DIR}/shaders/raw_preview.comp)
    get_target_property(RAW_HIGHLIGHT_SHADER_DIR raw_highlight_shader_contract INTERFACE_RAW_HIGHLIGHT_SHADER_DIR)
    if(NOT RAW_HIGHLIGHT_SHADER_DIR)
        message(FATAL_ERROR "raw_highlight shader contract did not publish its shader directory")
    endif()
    set(SHADER_INC ${RAW_HIGHLIGHT_SHADER_DIR}/raw_highlight_post_rgb.glsl)
    set(SHADER_COMMON_DIR ${CMAKE_CURRENT_LIST_DIR}/../shader_common/shaders)
    set(LSC_SHADER_INC ${SHADER_COMMON_DIR}/lens_shading.glsl ${SHADER_COMMON_DIR}/cfa_common.glsl)
    set(SHADER_SPV ${CMAKE_BINARY_DIR}/raw_preview.comp.spv)
    set(CFA_STATE_SHADER_SRC ${CMAKE_SOURCE_DIR}/shaders/raw_preview_cfa_state.comp)
    set(CFA_STATE_SHADER_SPV ${CMAKE_BINARY_DIR}/raw_preview_cfa_state.comp.spv)
    set(HL_SEED_SHADER_SPV ${CMAKE_BINARY_DIR}/raw_preview_highlight_guide_seed.comp.spv)
    set(HL_PROPAGATE_SHADER_SPV ${CMAKE_BINARY_DIR}/raw_preview_highlight_guide_propagate.comp.spv)
    set(HL_SMOOTH_SHADER_SPV ${CMAKE_BINARY_DIR}/raw_preview_highlight_guide_smooth.comp.spv)
    set(HL_APPLY_SHADER_SPV ${CMAKE_BINARY_DIR}/raw_preview_highlight_apply.comp.spv)

    if(GLSLANG_VALIDATOR)
        add_custom_command(
            OUTPUT ${SHADER_SPV}
            COMMAND ${GLSLANG_VALIDATOR}
                -V
                -I${CMAKE_SOURCE_DIR}/shaders
                -I${RAW_HIGHLIGHT_SHADER_DIR} -I${SHADER_COMMON_DIR}
                ${SHADER_SRC}
                -o ${SHADER_SPV}
            DEPENDS ${SHADER_SRC} ${SHADER_INC} ${LSC_SHADER_INC}
            VERBATIM
        )
    elseif(GLSLC)
        add_custom_command(
            OUTPUT ${SHADER_SPV}
            COMMAND ${GLSLC}
                -fshader-stage=compute
                -I${CMAKE_SOURCE_DIR}/shaders
                -I${RAW_HIGHLIGHT_SHADER_DIR} -I${SHADER_COMMON_DIR}
                ${SHADER_SRC}
                -o ${SHADER_SPV}
            DEPENDS ${SHADER_SRC} ${SHADER_INC} ${LSC_SHADER_INC}
            VERBATIM
        )
    else()
        message(FATAL_ERROR "No GLSL-to-SPIR-V compiler found (glslangValidator or glslc)")
    endif()

    if(GLSLANG_VALIDATOR)
        add_custom_command(
            OUTPUT ${CFA_STATE_SHADER_SPV}
            COMMAND ${GLSLANG_VALIDATOR} -V -I${CMAKE_SOURCE_DIR}/shaders -I${RAW_HIGHLIGHT_SHADER_DIR} -I${SHADER_COMMON_DIR} ${CFA_STATE_SHADER_SRC} -o ${CFA_STATE_SHADER_SPV}
            DEPENDS ${CFA_STATE_SHADER_SRC} ${SHADER_INC} ${LSC_SHADER_INC}
            VERBATIM
        )
    elseif(GLSLC)
        add_custom_command(
            OUTPUT ${CFA_STATE_SHADER_SPV}
            COMMAND ${GLSLC} -fshader-stage=compute -I${CMAKE_SOURCE_DIR}/shaders -I${RAW_HIGHLIGHT_SHADER_DIR} -I${SHADER_COMMON_DIR} ${CFA_STATE_SHADER_SRC} -o ${CFA_STATE_SHADER_SPV}
            DEPENDS ${CFA_STATE_SHADER_SRC} ${SHADER_INC} ${LSC_SHADER_INC}
            VERBATIM
        )
    endif()

    # Guide chain sources live in raw_highlight; preview builds scale 1.
    set(HL_SRC_highlight_guide_seed ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_seed.comp)
    set(HL_SRC_highlight_guide_propagate ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_propagate.comp)
    set(HL_SRC_highlight_guide_smooth ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_smooth.comp)
    set(HL_SRC_highlight_guide_control ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_control.comp)
    set(HL_SRC_highlight_apply ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_apply_inplace.comp)
    set(HL_SRC_coloropp ${RAW_HIGHLIGHT_SHADER_DIR}/coloropp.comp)
    set(HL_SRC_coloropp_tone ${RAW_HIGHLIGHT_SHADER_DIR}/coloropp_tone.comp)
    set(HL_DEFS_coloropp -DRAWR_COLOROPP_POST_WB=1)
    foreach(HL_NAME highlight_guide_seed highlight_guide_propagate highlight_guide_smooth highlight_guide_control highlight_apply
                    coloropp coloropp_tone)
        set(HL_SRC ${HL_SRC_${HL_NAME}})
        set(HL_SPV ${CMAKE_BINARY_DIR}/raw_preview_${HL_NAME}.comp.spv)
        if(GLSLANG_VALIDATOR)
            add_custom_command(OUTPUT ${HL_SPV}
                COMMAND ${GLSLANG_VALIDATOR} -V -DRAWR_HL_SCALE=1 ${HL_DEFS_${HL_NAME}} -I${CMAKE_SOURCE_DIR}/shaders -I${RAW_HIGHLIGHT_SHADER_DIR} -I${SHADER_COMMON_DIR} ${HL_SRC} -o ${HL_SPV}
                DEPENDS ${HL_SRC} ${SHADER_INC} ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_sample.glsl
                VERBATIM)
        else()
            add_custom_command(OUTPUT ${HL_SPV}
                COMMAND ${GLSLC} -fshader-stage=compute -DRAWR_HL_SCALE=1 ${HL_DEFS_${HL_NAME}} -I${CMAKE_SOURCE_DIR}/shaders -I${RAW_HIGHLIGHT_SHADER_DIR} -I${SHADER_COMMON_DIR} ${HL_SRC} -o ${HL_SPV}
                DEPENDS ${HL_SRC} ${SHADER_INC} ${RAW_HIGHLIGHT_SHADER_DIR}/highlight_guide_sample.glsl
                VERBATIM)
        endif()
    endforeach()

    add_custom_target(raw_preview_shader ALL DEPENDS ${SHADER_SPV} ${CFA_STATE_SHADER_SPV} ${HL_SEED_SHADER_SPV} ${HL_PROPAGATE_SHADER_SPV} ${HL_SMOOTH_SHADER_SPV} ${HL_APPLY_SHADER_SPV}
        ${CMAKE_BINARY_DIR}/raw_preview_coloropp.comp.spv ${CMAKE_BINARY_DIR}/raw_preview_coloropp_tone.comp.spv
        ${CMAKE_BINARY_DIR}/raw_preview_highlight_guide_control.comp.spv)

    add_executable(raw_preview_validate
        validation/raw_preview_validation.cpp
    )
    target_link_libraries(raw_preview_validate PRIVATE raw_preview ${VULKAN_LIB})
    add_dependencies(raw_preview_validate raw_preview_shader)

    add_executable(raw_preview_benchmark
        bench/raw_preview_benchmark.cpp
    )
    target_link_libraries(raw_preview_benchmark PRIVATE raw_preview ${VULKAN_LIB})
    add_dependencies(raw_preview_benchmark raw_preview_shader)

    add_executable(raw_preview_cfa_state_validate
        validation/cfa_state_validation.cpp
    )
    target_link_libraries(raw_preview_cfa_state_validate PRIVATE raw_preview ${VULKAN_LIB})
    add_dependencies(raw_preview_cfa_state_validate raw_preview_shader)

    add_executable(raw_preview_cfa_state_benchmark
        bench/cfa_state_benchmark.cpp
    )
    target_link_libraries(raw_preview_cfa_state_benchmark PRIVATE raw_preview ${VULKAN_LIB})
    add_dependencies(raw_preview_cfa_state_benchmark raw_preview_shader)
endif()
