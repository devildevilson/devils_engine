if (NOT DEFINED DEVILS_RUNTIME_FILE)
  message(FATAL_ERROR "DEVILS_RUNTIME_FILE is not defined")
endif()

if (NOT DEFINED DEVILS_RUNTIME_OUTPUT_DIR)
  message(FATAL_ERROR "DEVILS_RUNTIME_OUTPUT_DIR is not defined")
endif()

file(GET_RUNTIME_DEPENDENCIES
  EXECUTABLES "${DEVILS_RUNTIME_FILE}"
  RESOLVED_DEPENDENCIES_VAR resolved_dependencies
  UNRESOLVED_DEPENDENCIES_VAR unresolved_dependencies
  PRE_EXCLUDE_REGEXES
    "linux-vdso\\.so.*"
    "ld-linux.*"
    "libc\\.so.*"
    "libm\\.so.*"
    "libdl\\.so.*"
    "libpthread\\.so.*"
    "libresolv\\.so.*"
    "librt\\.so.*"
    "libutil\\.so.*"
)

set(copied_dependency_files)
foreach(dependency IN LISTS resolved_dependencies)
  execute_process(
    COMMAND readelf -d "${dependency}"
    OUTPUT_VARIABLE dependency_dynamic_section
    ERROR_QUIET
  )

  string(REGEX MATCH "Library soname: \\[[^]]+\\]" soname_match "${dependency_dynamic_section}")
  if (soname_match)
    string(REGEX REPLACE ".*\\[([^]]+)\\].*" "\\1" output_name "${soname_match}")
  else()
    get_filename_component(output_name "${dependency}" NAME)
  endif()

  execute_process(
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      "${dependency}"
      "${DEVILS_RUNTIME_OUTPUT_DIR}/${output_name}"
    COMMAND_ERROR_IS_FATAL ANY
  )
  list(APPEND copied_dependency_files "${DEVILS_RUNTIME_OUTPUT_DIR}/${output_name}")
endforeach()

file(GLOB runtime_output_files "${DEVILS_RUNTIME_OUTPUT_DIR}/*.so*")
foreach(runtime_output_file IN LISTS runtime_output_files)
  list(FIND copied_dependency_files "${runtime_output_file}" copied_dependency_index)
  if (IS_SYMLINK "${runtime_output_file}" OR copied_dependency_index EQUAL -1)
    file(REMOVE "${runtime_output_file}")
  endif()
endforeach()

if (unresolved_dependencies)
  message(WARNING
    "Unresolved runtime dependencies for ${DEVILS_RUNTIME_FILE}: "
    "${unresolved_dependencies}"
  )
endif()
