if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
  message(FATAL_ERROR "CORING5_FUZZ requires clang (libFuzzer)")
endif()

function(coring5_fuzz name source lib)
  add_executable(${name} ${source})
  target_link_libraries(${name} PRIVATE ${lib})
  target_compile_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined -O1 -g
                                         -fno-omit-frame-pointer)
  target_link_options(${name} PRIVATE -fsanitize=fuzzer,address,undefined)
  # Corpus regressions run as a normal test: every file in corpus/<name>/ and
  # regressions/<name>/ is replayed once (libFuzzer: running with file args).
  file(GLOB _seeds ${CMAKE_CURRENT_SOURCE_DIR}/corpus/${name}/* ${CMAKE_CURRENT_SOURCE_DIR}/regressions/${name}/*)
  if(_seeds)
    add_test(NAME ${name}_replay COMMAND ${name} ${_seeds})
    set_tests_properties(${name}_replay PROPERTIES LABELS "fuzz-replay")
  endif()
endfunction()

coring5_fuzz(fuzz_http1_head  http1_head.cpp  coring5_proto_http1)
coring5_fuzz(fuzz_http1_body  http1_body.cpp  coring5_proto_http1)
coring5_fuzz(fuzz_config      config.cpp       coring5_config)
coring5_fuzz(fuzz_endpoint    endpoint.cpp     coring5_base)
