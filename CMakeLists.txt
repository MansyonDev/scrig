cmake_minimum_required(VERSION 3.20)
project(scrig LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_CONFIGURATION_TYPES AND NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
  set_property(CACHE CMAKE_BUILD_TYPE PROPERTY STRINGS "Debug" "Release" "RelWithDebInfo" "MinSizeRel")
endif()

option(SCRIG_USE_RANDOMX "Enable RandomX hashing (required for real Snap Coin mining)" ON)
option(SCRIG_ENABLE_LTO "Enable link-time optimization" ON)
option(SCRIG_MAX_PERF_TUNING "Enable aggressive CPU-specific Release tuning (recommended for local mining builds)" ON)
option(SCRIG_PORTABLE_BINARY "Build portable binaries for broader CPU compatibility" ON)
set(SCRIG_PGO "OFF" CACHE STRING "Profile-guided optimization mode (OFF, GENERATE, USE)")
set_property(CACHE SCRIG_PGO PROPERTY STRINGS OFF GENERATE USE)
set(RANDOMX_ROOT "" CACHE PATH "Path prefix where RandomX is installed (contains include/ and lib/)")

set(SCRIG_WARNING_FLAGS "")
if(MSVC)
  list(APPEND SCRIG_WARNING_FLAGS /W4 /permissive-)
else()
  list(APPEND SCRIG_WARNING_FLAGS -Wall -Wextra -Wpedantic -Wconversion)
endif()

if(SCRIG_ENABLE_LTO)
  include(CheckIPOSupported)
  check_ipo_supported(RESULT IPO_OK OUTPUT IPO_MSG)
  if(IPO_OK)
    set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
  else()
    message(STATUS "IPO/LTO disabled: ${IPO_MSG}")
  endif()
endif()

add_executable(scrig
  src/main.cpp
  src/config.cpp
  src/json.cpp
  src/crypto.cpp
  src/types.cpp
  src/node_client.cpp
  src/miner.cpp
  src/ui.cpp
  src/perf.cpp
)

if(APPLE)
  target_sources(scrig PRIVATE src/macos/platform.cpp)
  target_sources(scrig PRIVATE src/macos/runtime_platform.cpp)
  target_sources(scrig PRIVATE src/macos/net_platform.cpp)
  target_sources(scrig PRIVATE src/macos/perf_platform.cpp)
  target_sources(scrig PRIVATE src/macos/crypto_platform.cpp)
elseif(WIN32)
  target_sources(scrig PRIVATE src/windows/platform.cpp)
  target_sources(scrig PRIVATE src/windows/runtime_platform.cpp)
  target_sources(scrig PRIVATE src/windows/net_platform.cpp)
  target_sources(scrig PRIVATE src/windows/perf_platform.cpp)
  target_sources(scrig PRIVATE src/windows/crypto_platform.cpp)
elseif(UNIX)
  target_sources(scrig PRIVATE src/linux/platform.cpp)
  target_sources(scrig PRIVATE src/linux/runtime_platform.cpp)
  target_sources(scrig PRIVATE src/linux/net_platform.cpp)
  target_sources(scrig PRIVATE src/linux/perf_platform.cpp)
  target_sources(scrig PRIVATE src/linux/crypto_platform.cpp)
else()
  target_sources(scrig PRIVATE src/platform/platform.cpp)
  target_sources(scrig PRIVATE src/platform/runtime_platform.cpp)
  target_sources(scrig PRIVATE src/platform/net_platform.cpp)
  target_sources(scrig PRIVATE src/platform/perf_platform.cpp)
  target_sources(scrig PRIVATE src/platform/crypto_platform.cpp)
endif()

target_include_directories(scrig PRIVATE include)
target_compile_options(scrig PRIVATE ${SCRIG_WARNING_FLAGS})

target_compile_definitions(scrig PRIVATE SCRIG_VERSION=\"0.1.0\")
if(SCRIG_PORTABLE_BINARY)
  target_compile_definitions(scrig PRIVATE SCRIG_PORTABLE_BINARY=1)
  message(STATUS "Portable CPU mode enabled")
else()
  target_compile_definitions(scrig PRIVATE SCRIG_PORTABLE_BINARY=0)
  message(STATUS "Portable CPU mode disabled (native tuning allowed)")
endif()

string(TOUPPER "${SCRIG_PGO}" SCRIG_PGO_MODE)
set(SCRIG_PGO_COMPILE_FLAGS "")
set(SCRIG_PGO_LINK_FLAGS "")
if(NOT SCRIG_PGO_MODE STREQUAL "OFF")
  if(NOT SCRIG_PGO_MODE STREQUAL "GENERATE" AND NOT SCRIG_PGO_MODE STREQUAL "USE")
    message(FATAL_ERROR "SCRIG_PGO must be OFF, GENERATE, or USE")
  endif()

  if(MSVC)
    list(APPEND SCRIG_PGO_COMPILE_FLAGS /GL)
    if(SCRIG_PGO_MODE STREQUAL "GENERATE")
      list(APPEND SCRIG_PGO_LINK_FLAGS /LTCG:PGINSTRUMENT /GENPROFILE)
      message(STATUS "PGO generate mode enabled (MSVC)")
    else()
      list(APPEND SCRIG_PGO_LINK_FLAGS /LTCG:PGOPTIMIZE /USEPROFILE)
      message(STATUS "PGO use mode enabled (MSVC)")
    endif()
  elseif(UNIX AND NOT APPLE)
    set(SCRIG_PGO_DIR "${CMAKE_BINARY_DIR}/pgo-data")
    if(SCRIG_PGO_MODE STREQUAL "GENERATE")
      list(APPEND SCRIG_PGO_COMPILE_FLAGS "-fprofile-generate=${SCRIG_PGO_DIR}")
      list(APPEND SCRIG_PGO_LINK_FLAGS "-fprofile-generate=${SCRIG_PGO_DIR}")
      message(STATUS "PGO generate mode enabled (Linux): ${SCRIG_PGO_DIR}")
    else()
      list(APPEND SCRIG_PGO_COMPILE_FLAGS "-fprofile-use=${SCRIG_PGO_DIR}" -fprofile-correction)
      list(APPEND SCRIG_PGO_LINK_FLAGS "-fprofile-use=${SCRIG_PGO_DIR}")
      message(STATUS "PGO use mode enabled (Linux): ${SCRIG_PGO_DIR}")
    endif()
  else()
    message(WARNING "SCRIG_PGO=${SCRIG_PGO_MODE} is only supported on Windows/Linux in this project. Ignoring.")
  endif()
endif()

set(SCRIG_RELEASE_PERF_FLAGS "")
if(MSVC)
  list(APPEND SCRIG_RELEASE_PERF_FLAGS /O2 /Oi /Ot /GF /Gy)
  if(SCRIG_MAX_PERF_TUNING AND NOT SCRIG_PORTABLE_BINARY AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    list(APPEND SCRIG_RELEASE_PERF_FLAGS /arch:AVX2)
  endif()
else()
  list(APPEND SCRIG_RELEASE_PERF_FLAGS -O3 -fno-math-errno -fomit-frame-pointer)
  if(SCRIG_MAX_PERF_TUNING AND NOT SCRIG_PORTABLE_BINARY)
    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" SCRIG_CPU_ARCH)
    if(SCRIG_CPU_ARCH MATCHES "^(aarch64|arm64)")
      list(APPEND SCRIG_RELEASE_PERF_FLAGS -mcpu=native)
    else()
      list(APPEND SCRIG_RELEASE_PERF_FLAGS -march=native -mtune=native)
    endif()
  endif()
endif()

foreach(flag IN LISTS SCRIG_RELEASE_PERF_FLAGS)
  target_compile_options(scrig PRIVATE
    "$<$<AND:$<CONFIG:Release>,$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>>:${flag}>")
endforeach()

foreach(flag IN LISTS SCRIG_PGO_COMPILE_FLAGS)
  target_compile_options(scrig PRIVATE
    "$<$<AND:$<CONFIG:Release>,$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>>:${flag}>")
endforeach()
foreach(flag IN LISTS SCRIG_PGO_LINK_FLAGS)
  target_link_options(scrig PRIVATE "$<$<CONFIG:Release>:${flag}>")
endforeach()

if(SCRIG_USE_RANDOMX)
  set(RANDOMX_INCLUDE_DIR "")
  set(RANDOMX_LIBRARY "")
  set(RANDOMX_FROM_ROOT OFF)

  if(RANDOMX_ROOT)
    find_path(RANDOMX_INCLUDE_DIR randomx.h
      HINTS "${RANDOMX_ROOT}/include"
      NO_DEFAULT_PATH)
    find_library(RANDOMX_LIBRARY randomx
      HINTS
        "${RANDOMX_ROOT}/lib"
        "${RANDOMX_ROOT}/lib64"
      NO_DEFAULT_PATH)
    if(RANDOMX_INCLUDE_DIR AND RANDOMX_LIBRARY)
      set(RANDOMX_FROM_ROOT ON)
    endif()
  endif()

  if(NOT RANDOMX_INCLUDE_DIR OR NOT RANDOMX_LIBRARY)
    if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/RandomX/CMakeLists.txt")
      set(CMAKE_POLICY_DEFAULT_CMP0069 NEW)
      add_subdirectory(third_party/RandomX EXCLUDE_FROM_ALL)
      set(RANDOMX_INCLUDE_DIR "${CMAKE_SOURCE_DIR}/third_party/RandomX/src")
      set(RANDOMX_LIBRARY randomx)
    else()
      find_path(RANDOMX_INCLUDE_DIR randomx.h
        HINTS
          "${RANDOMX_ROOT}/include"
          "/opt/homebrew/include"
          "/usr/local/include"
          "/usr/include")
      find_library(RANDOMX_LIBRARY randomx
        HINTS
          "${RANDOMX_ROOT}/lib"
          "${RANDOMX_ROOT}/lib64"
          "/opt/homebrew/lib"
          "/usr/local/lib"
          "/usr/lib")
    endif()
  endif()

  if(RANDOMX_ROOT AND NOT RANDOMX_FROM_ROOT)
    message(WARNING "RANDOMX_ROOT was provided but RandomX was not found there. Falling back to vendored/system search.")
  endif()

  if(RANDOMX_INCLUDE_DIR AND RANDOMX_LIBRARY)
    if(RANDOMX_FROM_ROOT)
      message(STATUS "RandomX enabled from RANDOMX_ROOT=${RANDOMX_ROOT}")
    else()
      message(STATUS "RandomX enabled")
    endif()
    target_include_directories(scrig PRIVATE ${RANDOMX_INCLUDE_DIR})
    target_link_libraries(scrig PRIVATE ${RANDOMX_LIBRARY})
    target_compile_definitions(scrig PRIVATE SCRIG_HAVE_RANDOMX=1)
    if(TARGET randomx)
      if(MSVC)
        message(STATUS "RandomX extra target flags skipped on MSVC (avoids MASM instability)")
      else()
        foreach(flag IN LISTS SCRIG_RELEASE_PERF_FLAGS)
          target_compile_options(randomx PRIVATE
            "$<$<AND:$<CONFIG:Release>,$<OR:$<COMPILE_LANGUAGE:C>,$<COMPILE_LANGUAGE:CXX>>>:${flag}>")
        endforeach()
      endif()
    endif()
  else()
    message(WARNING "RandomX requested but not found. Building fallback hasher (not chain-compatible).")
  endif()
endif()

find_path(HWLOC_INCLUDE_DIR hwloc.h)
find_library(HWLOC_LIBRARY hwloc)
if(HWLOC_INCLUDE_DIR AND HWLOC_LIBRARY)
  target_include_directories(scrig PRIVATE ${HWLOC_INCLUDE_DIR})
  target_link_libraries(scrig PRIVATE ${HWLOC_LIBRARY})
  target_compile_definitions(scrig PRIVATE SCRIG_HAVE_HWLOC=1)
  message(STATUS "hwloc support enabled")
endif()

if(WIN32)
  target_link_libraries(scrig PRIVATE ws2_32)
endif()

if(APPLE)
  find_library(COREFOUNDATION_FRAMEWORK CoreFoundation)
  if(COREFOUNDATION_FRAMEWORK)
    target_link_libraries(scrig PRIVATE ${COREFOUNDATION_FRAMEWORK})
  endif()
endif()

if(UNIX AND NOT APPLE)
  find_path(NUMA_INCLUDE_DIR numa.h)
  find_library(NUMA_LIBRARY numa)
  if(NUMA_INCLUDE_DIR AND NUMA_LIBRARY)
    target_include_directories(scrig PRIVATE ${NUMA_INCLUDE_DIR})
    target_link_libraries(scrig PRIVATE ${NUMA_LIBRARY})
    target_compile_definitions(scrig PRIVATE SCRIG_HAVE_LIBNUMA=1)
    message(STATUS "NUMA support enabled")
  endif()
endif()
