cmake_minimum_required(VERSION 3.20)
project(scrig LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(BUILD_TESTING OFF CACHE BOOL "" FORCE)

set(RANDOMX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RANDOMX_BUILD_BENCHMARKS OFF CACHE BOOL "" FORCE)
set(RANDOMX_BUILD_SHARED OFF CACHE BOOL "" FORCE)

find_package(OpenSSL REQUIRED)

add_subdirectory(external/randomx EXCLUDE_FROM_ALL)

if (MSVC AND CMAKE_SIZEOF_VOID_P EQUAL 8)
    target_sources(randomx PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/external/randomx/src/jit_compiler_x86_static.asm
    )
    set_source_files_properties(
        ${CMAKE_CURRENT_SOURCE_DIR}/external/randomx/src/jit_compiler_x86_static.asm
        PROPERTIES LANGUAGE ASM_MASM
    )
endif()

add_executable(scrig
    src/main.cpp
    src/protocol/hashing_buffer.cpp
)

target_include_directories(scrig PRIVATE
    src
    external/randomx/src
)

target_link_libraries(scrig PRIVATE
    randomx
    OpenSSL::Crypto
)

if (MSVC)
    target_compile_options(scrig PRIVATE /O2 /arch:AVX2)
else()
    target_compile_options(scrig PRIVATE -O3 -march=native)
endif()
