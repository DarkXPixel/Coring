cmake_minimum_required(VERSION 3.30)

project(Coring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBURING REQUIRED liburing)

add_executable(exe src/main.cpp
    src/coring_uring/UringEngine.cpp
    include/coring/HTTPParser.hpp
    src/utility/CliParser.cpp
    src/coring/IOContext.cpp
    src/system/SystemInfo.cpp
    src/coring/Connection.cpp
    src/coring2/TcpConnection.cpp)


target_include_directories(exe PRIVATE ${LIBURING_INCLUDE_DIRS})
target_link_directories(exe PRIVATE ${LIBURING_LIBRARY_DIRS})
target_link_libraries(exe PRIVATE ${LIBURING_LIBRARIES})

target_include_directories(exe PRIVATE include)