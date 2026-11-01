cmake_minimum_required(VERSION 3.30)

project(Coring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)


option(CORING_BUILD_LEGACY "Build the legacy coring/coring2/3/4 targets" ON)

find_package(PkgConfig REQUIRED)
if(CORING_BUILD_LEGACY)
  find_package(Boost REQUIRED)
endif()

pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)


option(ENABLE_ASAN "Enable AddressSanitizer (Memory Leaks & Out of Bounds)" OFF)
option(ENABLE_TSAN "Enable ThreadSanitizer (Data Races)" OFF)


if(ENABLE_ASAN AND ENABLE_TSAN)
  message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer cannot be used together.")
endif()


if(CORING_BUILD_LEGACY)
add_executable(cli src/cli/main.cpp)


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
    src/Coring4/Engine.cpp
    src/Coring4/RouteTable.cpp)


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
endif()

# --- routing: tests & benchmark (cmake --build . --target routing_test) ---
option(CORING_BUILD_TESTS "Build routing tests and benchmark" ON)
if(CORING_BUILD_TESTS)
  enable_testing()
  add_library(coring4_routing STATIC src/Coring4/RouteTable.cpp)
  target_include_directories(coring4_routing PUBLIC include)
  target_compile_options(coring4_routing PRIVATE -Wall -Wextra)

  add_executable(routing_test tests/routing_test.cpp)
  target_link_libraries(routing_test PRIVATE coring4_routing)
  target_compile_options(routing_test PRIVATE -Wall -Wextra)
  add_test(NAME routing_test COMMAND routing_test)

  add_executable(routing_bench tests/routing_bench.cpp)
  target_link_libraries(routing_bench PRIVATE coring4_routing)
  target_compile_options(routing_bench PRIVATE -O3)

  if(ENABLE_ASAN)
    foreach(t coring4_routing routing_test)
      target_compile_options(${t} PRIVATE -fsanitize=address,undefined -g -O1 -fno-omit-frame-pointer)
      target_link_options(${t} PRIVATE -fsanitize=address,undefined)
    endforeach()
  endif()
  if(ENABLE_TSAN)
    foreach(t coring4_routing routing_test)
      target_compile_options(${t} PRIVATE -fsanitize=thread -g -O1)
      target_link_options(${t} PRIVATE -fsanitize=thread)
    endforeach()
  endif()
endif()


# --- Coring5 (the rewrite; see docs/) -----------------------------------------
option(CORING5_BUILD "Build Coring5 (coringd, coringctl, tests)" ON)
if(CORING5_BUILD)
  add_subdirectory(src/coring5)
  option(CORING5_BUILD_TESTS "Build Coring5 tests" ON)
  if(CORING5_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/coring5)
  endif()
endif()
