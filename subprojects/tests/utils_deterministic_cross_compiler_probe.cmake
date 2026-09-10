if (NOT DEFINED PROBE_SOURCE OR NOT DEFINED PROBE_INCLUDE_DIR OR
    NOT DEFINED PROBE_BINARY_DIR OR NOT DEFINED GXX OR NOT DEFINED CLANGXX)
  message(FATAL_ERROR "utils deterministic probe is missing required paths")
endif()

file(MAKE_DIRECTORY "${PROBE_BINARY_DIR}")
set(common_flags -std=c++23 -O3 -fno-fast-math -ffp-contract=off -I${PROBE_INCLUDE_DIR})
set(gcc_binary "${PROBE_BINARY_DIR}/utils_deterministic_gcc")
set(clang_binary "${PROBE_BINARY_DIR}/utils_deterministic_clang")

execute_process(
  COMMAND "${GXX}" ${common_flags} "${PROBE_SOURCE}" -o "${gcc_binary}"
  RESULT_VARIABLE gcc_result ERROR_VARIABLE gcc_error)
if (NOT gcc_result EQUAL 0)
  message(FATAL_ERROR "GCC deterministic corpus compile failed:\n${gcc_error}")
endif()

execute_process(
  COMMAND "${CLANGXX}" ${common_flags} "${PROBE_SOURCE}" -o "${clang_binary}"
  RESULT_VARIABLE clang_result ERROR_VARIABLE clang_error)
if (NOT clang_result EQUAL 0)
  message(FATAL_ERROR "Clang deterministic corpus compile failed:\n${clang_error}")
endif()

execute_process(COMMAND "${gcc_binary}"
  OUTPUT_FILE "${PROBE_BINARY_DIR}/gcc.hex" RESULT_VARIABLE gcc_run)
execute_process(COMMAND "${clang_binary}"
  OUTPUT_FILE "${PROBE_BINARY_DIR}/clang.hex" RESULT_VARIABLE clang_run)
if (NOT gcc_run EQUAL 0 OR NOT clang_run EQUAL 0)
  message(FATAL_ERROR "utils deterministic corpus executable failed")
endif()

execute_process(
  COMMAND "${CMAKE_COMMAND}" -E compare_files
    "${PROBE_BINARY_DIR}/gcc.hex" "${PROBE_BINARY_DIR}/clang.hex"
  RESULT_VARIABLE difference)
if (NOT difference EQUAL 0)
  message(FATAL_ERROR "utils deterministic math/sort differ between GCC and Clang")
endif()

file(SHA256 "${PROBE_BINARY_DIR}/gcc.hex" signature)
file(SIZE "${PROBE_BINARY_DIR}/gcc.hex" bytes)
message(STATUS "utils deterministic probe: ${bytes} bytes identical, SHA-256=${signature}")
