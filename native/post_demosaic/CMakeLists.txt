cmake_minimum_required(VERSION 3.22)
project(post_demosaic LANGUAGES CXX)

# Post-demosaic stages shared by stills, video and the DNG renderer:
# profiled denoise, white balance, highlight (both methods), false-colour
# correction and defringe. Embeds the SPIR-V it dispatches from its own
# shaders and from raw_highlight, raw_denoise and false_color_correction.
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
set(DEP_TARGET_vk_common rawr::vk_common)
set(DEP_TARGET_shader_common rawr::shader_common)
set(DEP_TARGET_raw_highlight raw_highlight::raw_highlight)
set(DEP_TARGET_raw_denoise raw_denoise::raw_denoise)
set(DEP_TARGET_false_color_correction fcc::false_color_correction)
foreach(DEP vk_common shader_common raw_highlight raw_denoise false_color_correction)
  if(NOT TARGET ${DEP_TARGET_${DEP}})
    add_subdirectory(${RAWR_NATIVE_DIR}/${DEP} ${CMAKE_CURRENT_BINARY_DIR}/${DEP})
  endif()
endforeach()

set(PD_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${PD_GEN})
set(SC ${RAWR_NATIVE_DIR}/shader_common/shaders)
set(HL ${RAWR_NATIVE_DIR}/raw_highlight/shaders)
set(DN ${RAWR_NATIVE_DIR}/raw_denoise/shaders)
set(FCC ${RAWR_NATIVE_DIR}/false_color_correction/shaders)
set(PD_HEADERS)
macro(pd_embed NAME SRC)
  embed_spirv(${NAME} ${SRC} ${NAME}_spv OUTPUT_DIR ${PD_GEN} HEADERS_VAR PD_HEADERS ${ARGN})
endmacro()
# Own stages (legacy unoptimized compile, as before).
pd_embed(still_apply_wb ${CMAKE_CURRENT_LIST_DIR}/shaders/still_apply_wb.comp FLAGS -I${HL}
  DEPENDS ${HL}/raw_highlight_post_rgb.glsl)
pd_embed(still_defringe ${CMAKE_CURRENT_LIST_DIR}/shaders/still_defringe.comp)
# Highlight: still/video geometry (RAWR_HL_SCALE=2).
pd_embed(still_coloropp ${HL}/coloropp.comp FLAGS -I${SC} DEPENDS ${SC}/lens_shading.glsl ${SC}/cfa_common.glsl)
pd_embed(still_coloropp_fused_wb ${HL}/coloropp.comp FLAGS -DRAWR_COLOROPP_FUSED_WB=1 -I${SC} -I${HL}
  DEPENDS ${SC}/lens_shading.glsl ${SC}/cfa_common.glsl ${HL}/raw_highlight_post_rgb.glsl)
pd_embed(still_coloropp_tone ${HL}/coloropp_tone.comp)
pd_embed(still_highlight_guide_seed ${HL}/highlight_guide_seed.comp FLAGS -DRAWR_HL_SCALE=2 -I${HL}
  DEPENDS ${HL}/raw_highlight_post_rgb.glsl)
pd_embed(still_highlight_guide_propagate ${HL}/highlight_guide_propagate.comp)
pd_embed(still_highlight_guide_smooth ${HL}/highlight_guide_smooth.comp)
pd_embed(still_highlight_recovery ${HL}/highlight_recover.comp FLAGS -DRAWR_HL_SCALE=2 -I${HL}
  DEPENDS ${HL}/raw_highlight_post_rgb.glsl ${HL}/highlight_guide_sample.glsl)
pd_embed(still_derive_clip_state ${HL}/highlight_clip_state.comp)
# Profiled wavelet denoise.
foreach(NAME denoise_img_precondition denoise_img_atrous denoise_img_threshold denoise_img_backtransform)
  pd_embed(${NAME} ${DN}/${NAME}.comp FLAGS --target-env=vulkan1.1 -O)
endforeach()
# False-colour correction.
foreach(NAME fcc_chroma_median fcc_reconstruct fcc_fused)
  pd_embed(${NAME} ${FCC}/${NAME}.comp FLAGS --target-env=vulkan1.1 -O -fshader-stage=compute)
endforeach()

foreach(STAGE prepare reduce reconstruct)
  pd_embed(lab_defringe_${STAGE} ${CMAKE_CURRENT_LIST_DIR}/shaders/lab_defringe_${STAGE}.comp
    FLAGS --target-env=vulkan1.1 -O -I${CMAKE_CURRENT_LIST_DIR}/shaders
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/shaders/lab_defringe_common.glsl)
endforeach()
add_library(post_demosaic STATIC src/PostDemosaicProcessor.cpp src/LabDefringe.cpp ${PD_HEADERS})
add_library(rawr::post_demosaic ALIAS post_demosaic)
target_include_directories(post_demosaic PUBLIC include PRIVATE ${PD_GEN})
target_link_libraries(post_demosaic
  PUBLIC rawr::vk_common rawr::shader_common raw_highlight::raw_highlight raw_denoise::raw_denoise
         fcc::false_color_correction ${VULKAN_LIB})
target_compile_features(post_demosaic PUBLIC cxx_std_17)

option(POST_DEMOSAIC_BUILD_TESTS "Build production Lab file validation/timing harness" OFF)
if(POST_DEMOSAIC_BUILD_TESTS)
  add_executable(lab_defringe_validate tests/lab_defringe_validate.cpp)
  target_include_directories(lab_defringe_validate PRIVATE ${RAWR_NATIVE_DIR}/vk_common/testing)
  target_link_libraries(lab_defringe_validate PRIVATE rawr::post_demosaic)
endif()
