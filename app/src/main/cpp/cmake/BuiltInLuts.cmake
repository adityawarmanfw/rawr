# rawr_embed_builtin_lut(<input .cube> <output .inc>)
#
# Embeds a built-in .cube as a raw string literal for tonemap::lut::parseCube,
# so built-in looks ship in the native library (no asset/filesDir plumbing).
# Editing the cube re-runs configure.
function(rawr_embed_builtin_lut INPUT OUTPUT)
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${INPUT}")
  file(READ "${INPUT}" cube)
  file(CONFIGURE OUTPUT "${OUTPUT}"
    CONTENT "// Generated from ${INPUT} by BuiltInLuts.cmake.\nR\"rawr_lut(${cube})rawr_lut\"\n" @ONLY)
endfunction()
