cmake_minimum_required(VERSION 3.30)

project(Coring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)


find_package(PkgConfig REQUIRED)
find_package(Boost REQUIRED)

pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)


option(ENABLE_ASAN "Enable AddressSanitizer (Memory Leaks & Out of Bounds)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer (Data Races)" OFF)


if(ENABLE_ASAN AND ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be used together.")
endif()



add_executable(exe src/main.cpp
    src/coring_uring/UringEngine.cpp
    include/coring/HTTPParser.hpp
    src/utility/CliParser.cpp
    src/coring/IOContext.cpp
    src/system/SystemInfo.cpp
    src/coring/Connection.cpp
    src/coring2/TcpConnection.cpp
    src/coring2/http1/http1_handler.cpp
    src/coring3/ThreadWorker.cpp
    src/Coring4/EngineTestClientSession.cpp
    src/Coring4/Engine.cpp)


if(ENABLE_ASAN)
    message(STATUS "Building with AddressSanitizer (ASan)")
    target_compile_options(exe PRIVATE -fsanitize=address -g -O1 -fno-omit-frame-pointer)
    target_link_options(exe PRIVATE -fsanitize=address)
endif()

if(ENABLE_TSAN)
    message(STATUS "Building with ThreadSanitizer (TSan)")
    target_compile_options(exe PRIVATE -fsanitize=thread -g -O1)
    target_link_options(exe PRIVATE -fsanitize=thread)
endif()


target_link_libraries(exe PRIVATE 
    PkgConfig::LIBURING Boost::boost
)

target_include_directories(exe PRIVATE include)