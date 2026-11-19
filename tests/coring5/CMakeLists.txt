find_package(GTest REQUIRED)
include(GoogleTest)

# Tests must not fail the build on warnings from gtest macros.
function(coring5_add_test name)
  cmake_parse_arguments(T "NEEDS_RING" "" "SOURCES;LIBS" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ${T_LIBS} GTest::gtest GTest::gtest_main)
  target_compile_features(${name} PRIVATE cxx_std_23)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(T_NEEDS_RING)
    # io_uring may be blocked (containers/seccomp): tests skip themselves then.
    gtest_discover_tests(${name} PROPERTIES LABELS "uring" DISCOVERY_TIMEOUT 30)
  else()
    gtest_discover_tests(${name} PROPERTIES LABELS "unit" DISCOVERY_TIMEOUT 30)
  endif()
endfunction()

# ---- pure unit tests (no kernel features needed) -----------------------------
coring5_add_test(coring5_unit_base   SOURCES unit/base_test.cpp   LIBS coring5_base)
coring5_add_test(coring5_unit_mem    SOURCES unit/handle_pool_test.cpp LIBS coring5_mem)
coring5_add_test(coring5_unit_optag  SOURCES unit/optag_test.cpp  LIBS coring5_io)
coring5_add_test(coring5_unit_timer  SOURCES unit/timer_wheel_test.cpp LIBS coring5_io)
coring5_add_test(coring5_unit_config SOURCES unit/config_test.cpp LIBS coring5_config)
coring5_add_test(coring5_unit_http1  SOURCES unit/http1_parser_test.cpp LIBS coring5_proto_http1)
coring5_add_test(coring5_unit_obs    SOURCES unit/obs_test.cpp    LIBS coring5_obs)
coring5_add_test(coring5_unit_relay  SOURCES unit/relay_test.cpp  LIBS coring5_proto_l4)

# ---- integration tests (real io_uring + sockets) -----------------------------
coring5_add_test(coring5_it_echo NEEDS_RING
  SOURCES integration/echo_test.cpp LIBS coring5_runtime)
coring5_add_test(coring5_it_reload NEEDS_RING
  SOURCES integration/reload_test.cpp LIBS coring5_runtime)
coring5_add_test(coring5_it_l4 NEEDS_RING
  SOURCES integration/l4_test.cpp LIBS coring5_runtime)

# ---- fuzzing (opt-in; needs clang + libFuzzer) ---------------------------------
option(CORING5_FUZZ "Build libFuzzer targets (requires clang)" OFF)
if(CORING5_FUZZ)
  add_subdirectory(fuzz)
endif()
