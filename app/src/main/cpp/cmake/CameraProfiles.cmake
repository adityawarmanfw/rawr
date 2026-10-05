# rawr_embed_camera_profiles(<output .inc>)
#
# Embeds every camera/profiles/*.json as a raw string literal, one per line, in
# file-name order, for CameraRouting.cpp to parse. Adding or editing a profile
# re-runs configure, so a new device needs only a JSON file.
function(rawr_embed_camera_profiles OUTPUT)
  set(dir "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/../camera/profiles")
  file(GLOB profiles CONFIGURE_DEPENDS "${dir}/*.json")
  list(SORT profiles)
  set(body "// Generated from camera/profiles/*.json by CameraProfiles.cmake.\n")
  foreach(profile IN LISTS profiles)
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${profile}")
    file(READ "${profile}" json)
    string(APPEND body "R\"rawr_profile(${json})rawr_profile\",\n")
  endforeach()
  file(CONFIGURE OUTPUT "${OUTPUT}" CONTENT "${body}" @ONLY)
endfunction()
