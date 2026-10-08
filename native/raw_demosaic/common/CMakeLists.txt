# Embedded demosaic shaders: SPIR-V for RCD, VNG4, Dual and quadfix compiled
# with the production workgroup sizes, exposed as ShaderProviders
# (raw_demosaic/EmbeddedShaders.hpp). Standalone validation builds load
# SPIR-V from files instead and do not need this target.
if(NOT GLSLC)
  find_program(GLSLC glslc REQUIRED)
endif()
if(NOT COMMAND embed_spirv)
  include(${CMAKE_CURRENT_LIST_DIR}/../../vk_common/cmake/EmbedSpirv.cmake)
endif()

set(DM ${CMAKE_CURRENT_LIST_DIR}/..)
set(DM_GEN ${CMAKE_CURRENT_BINARY_DIR}/generated)
file(MAKE_DIRECTORY ${DM_GEN})
set(DM_HEADERS)
macro(dm_embed NAME SRC)
  embed_spirv(${NAME} ${SRC} ${NAME}_spv OUTPUT_DIR ${DM_GEN} HEADERS_VAR DM_HEADERS ${ARGN})
endmacro()

set(RCD_SH ${DM}/rcd/shaders)
foreach(SPEC "rcd_direction;4;32" "rcd_green;4;32" "rcd_diagonal;32;8" "rcd_green_sites;8;32" "rcd_export;4;32")
  list(GET SPEC 0 NAME)
  list(GET SPEC 1 X)
  list(GET SPEC 2 Y)
  dm_embed(${NAME} ${RCD_SH}/${NAME}.comp
    FLAGS --target-env=vulkan1.1 -O -DRCD_LOCAL_X=${X} -DRCD_LOCAL_Y=${Y} -I${RCD_SH}
    DEPENDS ${RCD_SH}/rcd_common.glsl)
endforeach()

foreach(NAME rcd_balance rcd_balance_reduce)
  dm_embed(${NAME} ${RCD_SH}/${NAME}.comp FLAGS --target-env=vulkan1.1 -O -I${RCD_SH} DEPENDS ${RCD_SH}/rcd_common.glsl)
endforeach()

set(VNG_SH ${DM}/vng4/shaders)
# vng4_green_diagnostic is a validation-only shader and is not embedded.
foreach(SPEC "vng4_linear;16;16" "vng4_green;16;16" "vng4_export;16;16" "vng4_export_blend;16;16")
  list(GET SPEC 0 NAME)
  list(GET SPEC 1 X)
  list(GET SPEC 2 Y)
  dm_embed(${NAME} ${VNG_SH}/${NAME}.comp FLAGS --target-env=vulkan1.1 -O -DVNG4_WGX=${X} -DVNG4_WGY=${Y} -I${VNG_SH})
endforeach()

foreach(NAME dual_luminance dual_mask dual_gauss_h dual_gauss_v dual_blend dual_auto_tiles dual_auto_select
             dual_auto_refine dual_auto_threshold)
  dm_embed(${NAME} ${DM}/dual/shaders/${NAME}.comp FLAGS --target-env=vulkan1.1 -O -fshader-stage=compute)
endforeach()

# quadfix: 7 sources -> 9 shaders (gauss/mask also build with QUADFIX_PASS_V=1).
set(QF_SH ${DM}/quadfix/shaders)
foreach(SPEC "quadfix_guide;quadfix_guide" "quadfix_guide_fast;quadfix_guide_fast" "quadfix_energy;quadfix_energy"
             "quadfix_demod;quadfix_demod" "quadfix_gauss_h;quadfix_gauss" "quadfix_gauss_v;quadfix_gauss;-DQUADFIX_PASS_V=1"
             "quadfix_mask_h;quadfix_mask" "quadfix_mask_v;quadfix_mask;-DQUADFIX_PASS_V=1" "quadfix_blend;quadfix_blend")
  list(GET SPEC 0 NAME)
  list(GET SPEC 1 SRC)
  list(LENGTH SPEC N)
  set(EXTRA)
  if(N GREATER 2)
    list(GET SPEC 2 EXTRA)
  endif()
  dm_embed(${NAME} ${QF_SH}/${SRC}.comp
    FLAGS --target-env=vulkan1.1 -O -DQUADFIX_LOCAL_X=16 -DQUADFIX_LOCAL_Y=16 ${EXTRA} -I${QF_SH}
    DEPENDS ${QF_SH}/quadfix_common.glsl ${QF_SH}/quadfix_params.glsl)
endforeach()

add_library(raw_demosaic_embedded_shaders STATIC src/EmbeddedShaders.cpp ${DM_HEADERS})
add_library(raw_demosaic::embedded_shaders ALIAS raw_demosaic_embedded_shaders)
target_include_directories(raw_demosaic_embedded_shaders PUBLIC include PRIVATE ${DM_GEN})
target_link_libraries(raw_demosaic_embedded_shaders PUBLIC rcd::rcd vng4::vng4 dual::dual_demosaic quadfix::quadfix)
target_compile_features(raw_demosaic_embedded_shaders PUBLIC cxx_std_17)
