cmake_minimum_required(VERSION 3.16)
project(netlib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# spdlog
find_package(spdlog CONFIG REQUIRED)

# asio (header-only, no CMake config in apt)
find_path(ASIO_INCLUDE_DIR asio.hpp)
if(NOT ASIO_INCLUDE_DIR)
    message(FATAL_ERROR "asio not found. Install: sudo apt install libasio-dev")
endif()

find_package(Threads REQUIRED)

# 源文件
set(SOURCES
    netlib.cpp
    session.cpp
    server.cpp
)

set(HEADERS
    config.h
    protocol.h
    work_queue.h
    buffer_pool.h
    shared_frame_pool.h
    session.h
    server.h
)

add_executable(netlib ${SOURCES} ${HEADERS})

target_include_directories(netlib PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASIO_INCLUDE_DIR}
)

target_link_libraries(netlib PRIVATE
    spdlog::spdlog
    Threads::Threads
)

target_compile_definitions(netlib PRIVATE ASIO_STANDALONE)
