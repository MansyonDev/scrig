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

target_include_directories(scrig PRIVATE include)
target_compile_options(scrig PRIVATE ${SCRIG_WARNING_FLAGS})

target_compile_definitions(scrig PRIVATE SCRIG_VERSION=\"0.1.0\")

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
  else()
    message(WARNING "RandomX requested but not found. Building fallback hasher (not chain-compatible).")
  endif()
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
