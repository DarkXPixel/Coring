cmake_minimum_required(VERSION 3.12)

project(Coring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

find_package(PkgConfig REQUIRED)

pkg_check_modules(LIBURING REQUIRED liburing)

add_executable(exe src/main.cpp)

target_include_directories(exe PRIVATE ${LIBURING_INCLUDE_DIRS})
target_link_directories(exe PRIVATE ${LIBURING_LIBRARY_DIRS})
target_link_libraries(exe PRIVATE ${LIBURING_LIBRARIES})