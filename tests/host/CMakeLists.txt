# tests/host/CMakeLists.txt
# PC 上编译跑 pdu_codec 测试, 不需要 ESP-IDF / Arduino
#
#   cd tests/host
#   cmake -B build && cmake --build build
#   ./build/test_pdu_codec
cmake_minimum_required(VERSION 3.10)
project(test_pdu_codec CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_executable(test_pdu_codec
    ../../src/pdu_codec.cpp
    test_pdu_codec.cpp
)

target_include_directories(test_pdu_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../src)

if(MSVC)
    target_compile_options(test_pdu_codec PRIVATE /W4)
else()
    target_compile_options(test_pdu_codec PRIVATE -Wall -Wextra -Wpedantic)
endif()
