cmake_minimum_required(VERSION 3.20)
project(scrig LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(OpenSSL REQUIRED)

add_executable(scrig
    src/main.cpp
    src/protocol/hashing_buffer.cpp
)

target_include_directories(scrig PRIVATE src)
target_link_libraries(scrig PRIVATE OpenSSL::Crypto)
