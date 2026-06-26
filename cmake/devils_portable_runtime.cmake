function(devils_set_local_rpath target)
  if (UNIX AND NOT APPLE)
    target_link_options(${target} PRIVATE "LINKER:--disable-new-dtags")
    set_target_properties(${target} PROPERTIES
      BUILD_RPATH "\$ORIGIN"
      INSTALL_RPATH "\$ORIGIN"
    )
  endif()
endfunction()

function(devils_bundle_runtime_dependencies target)
  devils_set_local_rpath(${target})

  if (UNIX AND NOT APPLE)
    add_custom_command(TARGET ${target} POST_BUILD
      COMMAND ${CMAKE_COMMAND}
        -DDEVILS_RUNTIME_FILE=$<TARGET_FILE:${target}>
        -DDEVILS_RUNTIME_OUTPUT_DIR=$<TARGET_FILE_DIR:${target}>
        -P "${CMAKE_SOURCE_DIR}/cmake/devils_copy_runtime_dependencies.cmake"
      COMMENT "Copying Linux runtime dependencies for ${target}"
      VERBATIM
    )
  endif()
endfunction()
