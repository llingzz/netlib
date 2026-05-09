cmake_minimum_required(VERSION 3.16)
project(tcpserver VERSION 1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Dependencies
# ---------------------------------------------------------------------------
find_package(asio CONFIG REQUIRED)
find_package(spdlog CONFIG REQUIRED)
find_package(Threads REQUIRED)

# ---------------------------------------------------------------------------
# Executable
# ---------------------------------------------------------------------------
add_executable(tcpserver
    tcpserver.cpp
    tcpserver.h
)

target_link_libraries(tcpserver PRIVATE
    asio::asio
    spdlog::spdlog
    Threads::Threads
)

# ---------------------------------------------------------------------------
# Platform-specific
# ---------------------------------------------------------------------------
if(WIN32)
    # Suppress asio _WIN32_WINNT default warning
    target_compile_definitions(tcpserver PRIVATE _WIN32_WINNT=0x0601)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    # asio may need librt on older glibc for timerfd etc.
    if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 10)
        target_link_libraries(tcpserver PRIVATE rt)
    endif()
endif()
