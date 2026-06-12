cmake_minimum_required(VERSION 3.16)
project(tcpserver LANGUAGES CXX)

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

# ── Core 源文件 ────────────────────────────────────────────────
set(CORE_SOURCES
    core/server.cpp
    core/session.cpp
)

set(CORE_HEADERS
    core/config.h
    core/protocol.h
    core/work_queue.h
    core/buffer_pool.h
    core/shared_frame_pool.h
    core/session.h
    core/server.h
)

# ── Framework 源文件 ───────────────────────────────────────────
set(FRAMEWORK_SOURCES
    application.cpp
    service.cpp
)

set(FRAMEWORK_HEADERS
    service.h
    middleware.h
    dispatcher.h
    application.h
    middleware/connection_limiter.h
    middleware/rate_limiter.h
    middleware/access_log.h
)

# ── netlib 静态库 (core engine + framework) ────────────────────
add_library(netlib STATIC
    ${CORE_SOURCES}
    ${FRAMEWORK_SOURCES}
    ${CORE_HEADERS}
    ${FRAMEWORK_HEADERS}
)

target_include_directories(netlib PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASIO_INCLUDE_DIR}
)

target_link_libraries(netlib PUBLIC
    spdlog::spdlog
    Threads::Threads
)

target_compile_definitions(netlib PUBLIC ASIO_STANDALONE)

# ── Example 源文件 ────────────────────────────────────────────
set(EXAMPLE_SOURCES
    examples/echo/echo_service.cpp
    examples/chat/chat_service.cpp
    examples/dispatcher/dispatcher_service.cpp
)

set(EXAMPLE_HEADERS
    examples/echo/echo_service.h
    examples/chat/chat_service.h
    examples/dispatcher/dispatcher_service.h
)

# ── tcpserver 统一入口 (core + framework + examples) ──────────
add_executable(tcpserver
    tcpserver.cpp
    ${CORE_SOURCES}
    ${FRAMEWORK_SOURCES}
    ${EXAMPLE_SOURCES}
    ${CORE_HEADERS}
    ${FRAMEWORK_HEADERS}
    ${EXAMPLE_HEADERS}
)

target_include_directories(tcpserver PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${ASIO_INCLUDE_DIR}
)

target_link_libraries(tcpserver PRIVATE
    spdlog::spdlog
    Threads::Threads
)

target_compile_definitions(tcpserver PRIVATE ASIO_STANDALONE)
