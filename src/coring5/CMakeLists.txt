# Coring5: one static library per layer. Links go DOWNWARD only (docs/ARCHITECTURE.md,
# "Layering"). tools/check_layering.py verifies the #include graph matches.
#
#   apps -> runtime -> {handlers, proto, upstream, tls, control} -> io/net/config/routing -> mem -> base

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBURING REQUIRED IMPORTED_TARGET liburing)
find_package(Threads REQUIRED)

set(CORING5_INCLUDE_DIR ${PROJECT_SOURCE_DIR}/include)

include(${PROJECT_SOURCE_DIR}/cmake/Coring5Warnings.cmake)

function(coring5_module name)
  cmake_parse_arguments(M "" "" "SOURCES;DEPS;SYSTEM_DEPS" ${ARGN})
  if(M_SOURCES)
    add_library(coring5_${name} STATIC ${M_SOURCES})
    set(_scope PUBLIC)
  else()
    add_library(coring5_${name} INTERFACE)
    set(_scope INTERFACE)
  endif()
  add_library(coring5::${name} ALIAS coring5_${name})
  target_include_directories(coring5_${name} ${_scope} ${CORING5_INCLUDE_DIR})
  target_compile_features(coring5_${name} ${_scope} cxx_std_23)
  if(M_SOURCES)
    coring5_enable_warnings(coring5_${name})
  endif()
  foreach(d IN LISTS M_DEPS)
    target_link_libraries(coring5_${name} ${_scope} coring5_${d})
  endforeach()
  foreach(d IN LISTS M_SYSTEM_DEPS)
    target_link_libraries(coring5_${name} ${_scope} ${d})
  endforeach()
endfunction()

# ---- foundations -------------------------------------------------------------
coring5_module(base SOURCES base/Endpoint.cpp)
coring5_module(mem  SOURCES mem/Region.cpp DEPS base)

# ---- no kernel ring needed (testable anywhere) --------------------------------
coring5_module(config SOURCES config/Loader.cpp config/Diff.cpp DEPS base)
find_package(tomlplusplus QUIET)
if(tomlplusplus_FOUND)
  target_link_libraries(coring5_config PUBLIC tomlplusplus::tomlplusplus)
endif()                     # else: <toml++/toml.hpp> from the system include path

coring5_module(obs SOURCES obs/Metrics.cpp DEPS base)

# ---- io_uring layer ------------------------------------------------------------
coring5_module(io
  SOURCES io/Ring.cpp io/BufferRing.cpp io/TimerWheel.cpp io/Connection.cpp
  DEPS base mem config obs
  SYSTEM_DEPS PkgConfig::LIBURING)
coring5_module(net SOURCES net/ListenSocket.cpp DEPS base io)

# ---- protocols (sans-IO where possible) ---------------------------------------
coring5_module(proto_http1 SOURCES proto/http1/Parser.cpp DEPS base config)
coring5_module(proto_l4    SOURCES proto/l4/EchoConnection.cpp DEPS io)
coring5_module(upstream)    # interface-only until M3
coring5_module(tls)         # interface-only until M5
coring5_module(handlers DEPS proto_http1 upstream)   # interface-only until M4

# ---- runtime + control --------------------------------------------------------
coring5_module(control SOURCES control/ControlServer.cpp SYSTEM_DEPS Threads::Threads)
coring5_module(runtime
  SOURCES
    runtime/Mailbox.cpp runtime/Worker.cpp runtime/WorkerGroup.cpp
    runtime/ConfigManager.cpp runtime/Daemon.cpp
  DEPS io net proto_l4 control config obs
  SYSTEM_DEPS Threads::Threads)

# ---- applications ----------------------------------------------------------------
add_executable(coringd apps/coringd/main.cpp)
target_link_libraries(coringd PRIVATE coring5_runtime)
coring5_enable_warnings(coringd)

add_executable(coringctl apps/coringctl/main.cpp)
target_compile_features(coringctl PRIVATE cxx_std_23)
coring5_enable_warnings(coringctl)

install(TARGETS coringd coringctl RUNTIME DESTINATION sbin)
