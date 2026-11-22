find_package(GTest REQUIRED)
include(GoogleTest)

# Tests must not fail the build on warnings from gtest macros.
function(coring5_add_test name)
  cmake_parse_arguments(T "NEEDS_RING" "" "SOURCES;LIBS;DEFINES" ${ARGN})
  add_executable(${name} ${T_SOURCES})
  target_link_libraries(${name} PRIVATE ${T_LIBS} GTest::gtest GTest::gtest_main)
  target_compile_features(${name} PRIVATE cxx_std_23)
  target_compile_options(${name} PRIVATE -Wall -Wextra -Wno-unused-parameter)
  target_include_directories(${name} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  if(T_DEFINES)
    target_compile_definitions(${name} PRIVATE ${T_DEFINES})
  endif()
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
coring5_add_test(coring5_unit_http    SOURCES unit/body_stream_test.cpp unit/exchange_test.cpp unit/exchange_random_test.cpp LIBS coring5_http)
coring5_add_test(coring5_unit_body_framing SOURCES unit/body_framing_test.cpp LIBS coring5_http coring5_proto_http1)
coring5_add_test(coring5_unit_chunked SOURCES unit/chunked_decoder_test.cpp LIBS coring5_http coring5_proto_http1)
coring5_add_test(coring5_unit_smuggling SOURCES unit/smuggling_corpus_test.cpp LIBS coring5_http coring5_proto_http1)
coring5_add_test(coring5_unit_response_writer SOURCES unit/response_writer_test.cpp LIBS coring5_proto_http1)
coring5_add_test(coring5_unit_handlers SOURCES unit/handlers_test.cpp LIBS coring5_http)
coring5_add_test(coring5_unit_reverse_proxy SOURCES unit/reverse_proxy_test.cpp LIBS coring5_http)
coring5_enable_warnings(coring5_unit_reverse_proxy)
coring5_add_test(coring5_unit_static_file SOURCES unit/static_file_test.cpp LIBS coring5_handlers)
coring5_enable_warnings(coring5_unit_static_file)
coring5_add_test(coring5_unit_server_session SOURCES unit/server_session_test.cpp LIBS coring5_proto_http1session coring5_http coring5_proto_http1)

# S2 Http1Conn lifecycle tests: compile the connection type with the fake ring
# (io/RingSeam.hpp, CORING5_FAKE_RING) so no kernel is needed. Http1Conn.cpp is
# built into the test directly (not linked from coring5_proto_http1conn) so the
# seam macro reaches it; the sans-IO libs are linked normally.
# The io sources the connection type needs (ConnCore, TimerWheel) are compiled
# into the test too, so EVERY translation unit sees the same IoContext (the fake
# ring). Linking coring5_io would mix the two definitions (ODR).
set(CORING5_FAKE_RING_IO_SOURCES
  ${PROJECT_SOURCE_DIR}/src/coring5/io/ConnCore.cpp
  ${PROJECT_SOURCE_DIR}/src/coring5/io/TimerWheel.cpp)
coring5_add_test(coring5_unit_http1_conn
  SOURCES unit/http1_conn_test.cpp ${PROJECT_SOURCE_DIR}/src/coring5/proto/http1conn/Http1Conn.cpp ${CORING5_FAKE_RING_IO_SOURCES}
  LIBS coring5_http coring5_proto_http1 coring5_proto_http1session coring5_mem coring5_obs coring5_config coring5_base
  DEFINES CORING5_FAKE_RING)
coring5_add_test(coring5_unit_http1_conn_fake
  SOURCES unit/http1_conn_fake_test.cpp ${PROJECT_SOURCE_DIR}/src/coring5/proto/http1conn/Http1Conn.cpp ${CORING5_FAKE_RING_IO_SOURCES}
  LIBS coring5_http coring5_proto_http1 coring5_proto_http1session coring5_mem coring5_obs coring5_config coring5_base
  DEFINES CORING5_FAKE_RING)

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
