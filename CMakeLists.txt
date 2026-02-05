cmake_minimum_required(VERSION 3.20)
project(scrig LANGUAGES C CXX ASM ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(RANDOMX_BUILD_TESTS OFF CACHE BOOL "" FORCE)
set(RANDOMX_BUILD_BENCHMARK OFF CACHE BOOL "" FORCE)
set(RANDOMX_BUILD_SHARED OFF CACHE BOOL "" FORCE)

add_subdirectory(external/randomx)

if (WIN32)
    enable_language(ASM_MASM)

    set(RANDOMX_MASM
        ${CMAKE_SOURCE_DIR}/external/randomx/src/jit_compiler_x86_static.asm
    )

    set_source_files_properties(
        ${RANDOMX_MASM}
        PROPERTIES
        LANGUAGE ASM_MASM
    )

    target_sources(randomx PRIVATE ${RANDOMX_MASM})
endif()

add_executable(scrig
    src/main.cpp
    src/util/time.cpp
    src/util/bytes.cpp
    src/net/socket.cpp
    src/net/framed_tcp.cpp
    src/snap/config.cpp
    src/snap/api.cpp
    src/snap/requests.cpp
    src/snap/types.cpp
    src/snap/base36.cpp
    src/rx/randomx_wrap.cpp
)

target_include_directories(scrig PRIVATE
    src
    external
    external/randomx/src
    external/nlohmann
)

find_package(Threads REQUIRED)

if (WIN32)
    target_link_libraries(scrig PRIVATE ws2_32)
endif()

target_link_libraries(scrig PRIVATE randomx Threads::Threads)

if (MSVC)
    target_compile_options(scrig PRIVATE /O2 /arch:AVX2)
else()
    target_compile_options(scrig PRIVATE -O3 -march=native)
endif()
