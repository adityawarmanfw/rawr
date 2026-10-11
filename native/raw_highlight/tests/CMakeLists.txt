cmake_minimum_required(VERSION 3.22)
project(highlight_fit_validation LANGUAGES CXX)
find_package(Vulkan REQUIRED)
find_program(GLSLC glslc REQUIRED)
set(SHADERS "${CMAKE_CURRENT_LIST_DIR}/../shaders" CACHE PATH "Production highlight shaders")
foreach(STAGE highlight_recover highlight_apply_inplace)
    if(STAGE STREQUAL "highlight_recover")
        set(SCALE 2)
    else()
        set(SCALE 1)
    endif()
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/${STAGE}.spv"
        COMMAND ${GLSLC} --target-env=vulkan1.1 -O -DRAWR_HL_SCALE=${SCALE}
            -I${SHADERS} "${SHADERS}/${STAGE}.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/${STAGE}.spv"
        DEPENDS "${SHADERS}/${STAGE}.comp" "${SHADERS}/raw_highlight_post_rgb.glsl"
            "${SHADERS}/highlight_guide_sample.glsl")
    list(APPEND SPV "${CMAKE_CURRENT_BINARY_DIR}/${STAGE}.spv")
endforeach()
foreach(SCALE 1 2)
    add_custom_command(OUTPUT "${CMAKE_CURRENT_BINARY_DIR}/highlight_seed_${SCALE}.spv"
        COMMAND ${GLSLC} --target-env=vulkan1.1 -O -DRAWR_HL_SCALE=${SCALE}
            -I${SHADERS} "${SHADERS}/highlight_guide_seed.comp" -o "${CMAKE_CURRENT_BINARY_DIR}/highlight_seed_${SCALE}.spv"
        DEPENDS "${SHADERS}/highlight_guide_seed.comp" "${SHADERS}/raw_highlight_post_rgb.glsl")
    list(APPEND SPV "${CMAKE_CURRENT_BINARY_DIR}/highlight_seed_${SCALE}.spv")
endforeach()
add_custom_target(highlight_shaders DEPENDS ${SPV})
add_executable(highlight_fit_validation highlight_fit_validation.cpp
    ../../image_scopes/tests/vulkan_test_context.cpp)
target_compile_features(highlight_fit_validation PRIVATE cxx_std_20)
target_include_directories(highlight_fit_validation PRIVATE ../../image_scopes/tests ../../vk_common/testing)
target_link_libraries(highlight_fit_validation PRIVATE Vulkan::Vulkan)
add_dependencies(highlight_fit_validation highlight_shaders)
enable_testing()
add_test(NAME highlight_fit_validation COMMAND highlight_fit_validation "${CMAKE_CURRENT_BINARY_DIR}")
