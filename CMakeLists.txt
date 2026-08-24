cmake_minimum_required(VERSION 4.3)

set(CMAKE_EXPERIMENTAL_CXX_IMPORT_STD "451f2fe2-a8a2-47c3-bc32-94786d8fc91b")
project(Coring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

set(CMAKE_CXX_SCAN_FOR_MODULES ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBURING REQUIRED liburing)

add_executable(exe src/main.cpp
    src/coring_uring/UringEngine.cpp
    include/coring/HTTPParser.hpp
    src/utility/CliParser.cpp
    src/coring/IOContext.cpp
    src/system/SystemInfo.cpp
    src/coring/Connection.cpp)

set_property(TARGET exe PROPERTY CXX_MODULE_STD ON)

target_include_directories(exe PRIVATE ${LIBURING_INCLUDE_DIRS})
target_link_directories(exe PRIVATE ${LIBURING_LIBRARY_DIRS})
target_link_libraries(exe PRIVATE ${LIBURING_LIBRARIES})

target_include_directories(exe PRIVATE include)