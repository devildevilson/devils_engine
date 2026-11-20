if (NOT DEFINED PROBE_SOURCE OR NOT DEFINED PROBE_INCLUDE_DIR OR
    NOT DEFINED PROBE_BINARY_DIR OR NOT DEFINED GXX OR NOT DEFINED CLANGXX)
  message(FATAL_ERROR "utils deterministic probe is missing required paths")
endif()

file(MAKE_DIRECTORY "${PROBE_BINARY_DIR}")
set(common_flags -std=c++23 -O3 -fno-fast-math -ffp-contract=off -I${PROBE_INCLUDE_DIR})
set(gcc_binary "${PROBE_BINARY_DIR}/utils_deterministic_gcc")
set(clang_binary "${PROBE_BINARY_DIR}/utils_deterministic_clang")
# On Windows clang compiles against the MSVC STL, which refuses a clang older than it expects
# (STL1000; the 2026-11 runner image pairs MSVC 14.52 with LLVM 20). This probe compares the bits
# two compilers produce, not toolchain support, so it takes Microsoft's documented escape hatch.
set(clang_flags)
if (CMAKE_HOST_WIN32)
  list(APPEND clang_flags -D_ALLOW_COMPILER_AND_STL_VERSION_MISMATCH)
endif()

# deterministic_math.cpp: double-функции живут в .cpp, а не в заголовке.
set(sources "${PROBE_SOURCE}")
if (DEFINED PROBE_EXTRA_SOURCE)
  list(APPEND sources "${PROBE_EXTRA_SOURCE}")
endif()

execute_process(
  COMMAND "${GXX}" ${common_flags} ${sources} -o "${gcc_binary}"
  RESULT_VARIABLE gcc_result ERROR_VARIABLE gcc_error)
if (NOT gcc_result EQUAL 0)
  message(FATAL_ERROR "GCC deterministic corpus compile failed:\n${gcc_error}")
endif()

execute_process(
  COMMAND "${CLANGXX}" ${common_flags} ${clang_flags} ${sources} -o "${clang_binary}"
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
