cmake_minimum_required(VERSION 3.30)

project(Coring LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
if(NOT DEFINED CMAKE_INTERPROCEDURAL_OPTIMIZATION)
  set(CMAKE_INTERPROCEDURAL_OPTIMIZATION ON)
endif()


option(CORING_BUILD_LEGACY "Build the legacy coring/coring2/3/4 targets" OFF)

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



# --- Coring5 (the rewrite; see docs/) -----------------------------------------
option(CORING5_BUILD "Build Coring5 (coringd, coringctl, tests)" ON)
if(CORING5_BUILD)
  add_subdirectory(src/coring5)
  option(CORING5_BUILD_TESTS "Build Coring5 tests" OFF)
  if(CORING5_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests/coring5)
  endif()
endif()
