cmake_minimum_required(VERSION 3.22)
project(video_pipeline LANGUAGES CXX)

# Recording-resolution RAW stage (Android): MHC demosaic, crop or anti-aliased
# 2x reduction, lens shading and clip state. Its output feeds
# native/post_demosaic.
find_library(VULKAN_LIB vulkan REQUIRED)
find_library(LOG_LIB log REQUIRED)
if(NOT GLSLC)
  find_program(GLSLC glslc REQUIRED)
endif()
if(NOT COMMAND embed_spirv)
  include(${CMAKE_CURRENT_LIST_DIR}/../vk_common/cmake/EmbedSpirv.cmake)
endif()
set(RAWR_NATIVE_DIR ${CMAKE_CURRENT_LIST_DIR}/..)
foreach(DEP vk_common shader_common)
  if(NOT TARGET rawr::${DEP})
    add_subdirectory(${RAWR_NATIVE_DIR}/${DEP} ${CMAKE_CURRENT_BINARY_DIR}/${DEP})
  endif()
endforeach()

# Shaders under shaders/; video_raw_core.glsl is also included by the fused
# RAW-input tonemap variant, so it stays on the consumer's include path.
set(VIDEO_PIPELINE_SHADER_DIR ${CMAKE_CURRENT_LIST_DIR}/shaders CACHE INTERNAL "")
set(VP_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${VP_GEN})
set(SC ${RAWR_NATIVE_DIR}/shader_common/shaders)
embed_spirv(video_demosaic ${VIDEO_PIPELINE_SHADER_DIR}/video_demosaic.comp video_demosaic_spv OUTPUT_DIR ${VP_GEN}
  FLAGS -I${SC} -I${VIDEO_PIPELINE_SHADER_DIR}
  DEPENDS ${VIDEO_PIPELINE_SHADER_DIR}/video_raw_core.glsl ${VIDEO_PIPELINE_SHADER_DIR}/video_mhc.glsl
          ${SC}/lens_shading.glsl ${SC}/cfa_common.glsl)

# Optimized SPIR-V: Adreno's compiler rejects the unoptimized strip pass
# (vkCreateComputePipelines VK_ERROR_UNKNOWN), as with the gain map shaders.
foreach(PASS h v)
  embed_spirv(video_downscale_${PASS} ${VIDEO_PIPELINE_SHADER_DIR}/video_downscale_${PASS}.comp
    video_downscale_${PASS}_spv OUTPUT_DIR ${VP_GEN}
    FLAGS -I${SC} -I${VIDEO_PIPELINE_SHADER_DIR} --target-env=vulkan1.1 -O
    DEPENDS ${VIDEO_PIPELINE_SHADER_DIR}/video_raw_core.glsl ${VIDEO_PIPELINE_SHADER_DIR}/video_mhc.glsl
            ${SC}/lens_shading.glsl ${SC}/cfa_common.glsl)
endforeach()

add_library(video_pipeline STATIC src/VideoDemosaic.cpp ${video_demosaic_HDR} ${video_downscale_h_HDR}
  ${video_downscale_v_HDR})
add_library(rawr::video_pipeline ALIAS video_pipeline)
target_include_directories(video_pipeline PUBLIC include PRIVATE ${VP_GEN})
target_link_libraries(video_pipeline PUBLIC rawr::vk_common ${VULKAN_LIB} PRIVATE ${LOG_LIB})
target_compile_features(video_pipeline PUBLIC cxx_std_17)
