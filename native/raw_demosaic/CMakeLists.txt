cmake_minimum_required(VERSION 3.22)
# Home for all raw demosaic algorithms: AMaZE, standalone RCD, standalone VNG4,
# the Dual RCD+VNG4 blend, and the quadfix adapt004 lattice pre-filter.
# Consumed as a single unit; individual targets remain addressable as
# amaze::amaze, rcd::rcd, vng4::vng4, dual::dual_demosaic, quadfix::quadfix.
project(raw_demosaic VERSION 1.0.0 LANGUAGES CXX)

option(RAW_DEMOSAIC_BUILD_AMAZE "Build AMaZE target" ON)
option(RAW_DEMOSAIC_BUILD_RCD "Build standalone RCD target" ON)
option(RAW_DEMOSAIC_BUILD_VNG4 "Build standalone VNG4 target" ON)
option(RAW_DEMOSAIC_BUILD_DUAL "Build Dual RCD+VNG4 blend target" ON)
option(RAW_DEMOSAIC_BUILD_QUADFIX "Build adapt004 quad-cell lattice pre-filter" ON)
option(RAW_DEMOSAIC_EMBED_SHADERS "Build raw_demosaic::embedded_shaders (needs glslc)" OFF)

if(RAW_DEMOSAIC_BUILD_AMAZE AND NOT TARGET amaze::amaze)
  set(AMAZE_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(amaze)
endif()

if(RAW_DEMOSAIC_BUILD_RCD AND NOT TARGET rcd::rcd)
  set(RCD_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  add_subdirectory(rcd)
endif()

if(RAW_DEMOSAIC_BUILD_VNG4 AND NOT TARGET vng4::vng4)
  set(VNG4_BUILD_TESTS OFF CACHE BOOL "" FORCE)
  set(VNG4_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
  add_subdirectory(vng4)
endif()

if(RAW_DEMOSAIC_BUILD_DUAL AND NOT TARGET dual::dual_demosaic)
  set(DUAL_USE_EXTERNAL_RCD ON CACHE BOOL "" FORCE)
  set(DUAL_USE_EXTERNAL_VNG4 ON CACHE BOOL "" FORCE)
  set(DUAL_BUILD_VALIDATION OFF CACHE BOOL "" FORCE)
  add_subdirectory(dual)
endif()

if(RAW_DEMOSAIC_BUILD_QUADFIX AND NOT TARGET quadfix::quadfix)
  add_subdirectory(quadfix)
endif()

if(RAW_DEMOSAIC_EMBED_SHADERS AND NOT TARGET raw_demosaic::embedded_shaders)
  add_subdirectory(common)
endif()
