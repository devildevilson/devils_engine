if (NOT DEFINED PROBE_SOURCE OR NOT DEFINED PROBE_BINARY_DIR OR
    NOT DEFINED GXX OR NOT DEFINED CLANGXX)
  message(FATAL_ERROR "network cross-compiler probe is missing required paths")
endif()

file(MAKE_DIRECTORY "${PROBE_BINARY_DIR}")
set(gcc_binary "${PROBE_BINARY_DIR}/native_float_gcc")
set(clang_binary "${PROBE_BINARY_DIR}/native_float_clang")
set(common_flags -std=c++23 -O3 -fno-fast-math)
if (DEFINED PROBE_ARCH_FLAG AND NOT PROBE_ARCH_FLAG STREQUAL "")
  list(APPEND common_flags "${PROBE_ARCH_FLAG}")
endif()

execute_process(
  COMMAND "${GXX}" ${common_flags} "${PROBE_SOURCE}" -o "${gcc_binary}"
  RESULT_VARIABLE gcc_compile_result
  ERROR_VARIABLE gcc_compile_error
)
if (NOT gcc_compile_result EQUAL 0)
  message(FATAL_ERROR "GCC corpus compile failed:\n${gcc_compile_error}")
endif()

execute_process(
  COMMAND "${CLANGXX}" ${common_flags} -stdlib=libc++ "${PROBE_SOURCE}" -o "${clang_binary}"
  RESULT_VARIABLE clang_libcxx_result
  ERROR_VARIABLE clang_libcxx_error
)
if (clang_libcxx_result EQUAL 0)
  set(clang_runtime "libc++")
else()
  execute_process(
    COMMAND "${CLANGXX}" ${common_flags} "${PROBE_SOURCE}" -o "${clang_binary}"
    RESULT_VARIABLE clang_compile_result
    ERROR_VARIABLE clang_compile_error
  )
  if (NOT clang_compile_result EQUAL 0)
    message(FATAL_ERROR
      "Clang corpus compile failed with libc++ and with its default runtime.\n"
      "libc++ error:\n${clang_libcxx_error}\n"
      "default error:\n${clang_compile_error}")
  endif()
  set(clang_runtime "libstdc++ fallback (libc++ development package unavailable)")
endif()

foreach(compiler IN ITEMS gcc clang)
  set(binary "${${compiler}_binary}")
  execute_process(
    COMMAND "${binary}"
    OUTPUT_FILE "${PROBE_BINARY_DIR}/${compiler}_first.hex"
    RESULT_VARIABLE first_result
  )
  execute_process(
    COMMAND "${binary}"
    OUTPUT_FILE "${PROBE_BINARY_DIR}/${compiler}_second.hex"
    RESULT_VARIABLE second_result
  )
  if (NOT first_result EQUAL 0 OR NOT second_result EQUAL 0)
    message(FATAL_ERROR "${compiler} corpus executable failed")
  endif()
  execute_process(
    COMMAND "${CMAKE_COMMAND}" -E compare_files
      "${PROBE_BINARY_DIR}/${compiler}_first.hex"
      "${PROBE_BINARY_DIR}/${compiler}_second.hex"
    RESULT_VARIABLE repeat_difference
  )
  if (NOT repeat_difference EQUAL 0)
    message(FATAL_ERROR "${compiler} corpus is not repeatable within one build")
  endif()
endforeach()

file(SHA256 "${PROBE_BINARY_DIR}/gcc_first.hex" gcc_sha256)
file(SHA256 "${PROBE_BINARY_DIR}/clang_first.hex" clang_sha256)
file(SIZE "${PROBE_BINARY_DIR}/gcc_first.hex" gcc_size)
file(SIZE "${PROBE_BINARY_DIR}/clang_first.hex" clang_size)
execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${PROBE_BINARY_DIR}/gcc_first.hex"
    "${PROBE_BINARY_DIR}/clang_first.hex"
  RESULT_VARIABLE cross_difference
)

if (cross_difference EQUAL 0)
  set(cross_result "bit-identical for this corpus")
  set(first_difference "none")
else()
  file(READ "${PROBE_BINARY_DIR}/gcc_first.hex" gcc_trace)
  file(READ "${PROBE_BINARY_DIR}/clang_first.hex" clang_trace)
  string(LENGTH "${gcc_trace}" gcc_trace_length)
  string(LENGTH "${clang_trace}" clang_trace_length)
  if (gcc_trace_length LESS clang_trace_length)
    set(shared_trace_length ${gcc_trace_length})
  else()
    set(shared_trace_length ${clang_trace_length})
  endif()

  set(first_character ${shared_trace_length})
  if (shared_trace_length GREATER 0)
    math(EXPR last_shared_character "${shared_trace_length} - 1")
    foreach(character RANGE 0 ${last_shared_character})
      string(SUBSTRING "${gcc_trace}" ${character} 1 gcc_character)
      string(SUBSTRING "${clang_trace}" ${character} 1 clang_character)
      if (NOT gcc_character STREQUAL clang_character)
        set(first_character ${character})
        break()
      endif()
    endforeach()
  endif()
  math(EXPR first_state_byte "${first_character} / 2")
  math(EXPR first_tick "${first_state_byte} / 28")
  math(EXPR first_tick_byte "${first_state_byte} % 28")
  set(first_difference "tick ${first_tick}, canonical state byte ${first_tick_byte}")
  set(cross_result "different at ${first_difference}; checkpoint correction is required")
endif()

file(WRITE "${PROBE_BINARY_DIR}/result.txt"
  "gcc_runtime=libstdc++\n"
  "clang_runtime=${clang_runtime}\n"
  "flags=${common_flags}\n"
  "gcc_bytes=${gcc_size}\n"
  "clang_bytes=${clang_size}\n"
  "gcc_sha256=${gcc_sha256}\n"
  "clang_sha256=${clang_sha256}\n"
  "first_difference=${first_difference}\n"
  "cross_result=${cross_result}\n")

message(STATUS
  "network native-float probe: Clang uses ${clang_runtime}; ${cross_result}; "
  "GCC=${gcc_sha256}; Clang=${clang_sha256}")
