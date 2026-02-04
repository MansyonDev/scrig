cmake_minimum_required(VERSION 3.20)
project(scrig LANGUAGES C CXX ASM_MASM)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_subdirectory(external/randomx)

add_executable(scrig
    src/main.cpp
    src/node_client.cpp
    src/miner.cpp
    src/protocol/hashing_buffer.cpp
)

target_include_directories(scrig PRIVATE
    src
    src/protocol
    external/randomx/src
    ${OPENSSL_INCLUDE_DIR}
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
