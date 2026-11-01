# Warning policy for Coring5 targets (legacy targets are left untouched).
#   CORING5_WERROR=ON  (default) turns warnings into errors for coring5 code.

option(CORING5_WERROR "Treat warnings as errors in coring5 targets" ON)

function(coring5_enable_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Woverloaded-virtual -Wnull-dereference -Wformat=2
    -Wno-unused-parameter)
  if(CORING5_WERROR)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
