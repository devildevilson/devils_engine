if (NOT DEFINED BINARY OR NOT DEFINED RESOURCE_ROOT OR NOT DEFINED PACKAGE_DIR)
  message(FATAL_ERROR "headless package needs BINARY, RESOURCE_ROOT and PACKAGE_DIR")
endif()

# Rebuild the staging directory from the current source tree. A stale causal file would
# change the content root even though the executable had been rebuilt correctly.
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}/bin" "${PACKAGE_DIR}/resources/modules")
file(COPY "${BINARY}" DESTINATION "${PACKAGE_DIR}/bin")
file(COPY "${RESOURCE_ROOT}/" DESTINATION "${PACKAGE_DIR}/resources/modules")

if (DEFINED RUNTIME_DLLS AND NOT RUNTIME_DLLS STREQUAL "")
  string(REPLACE "|" ";" runtime_dlls "${RUNTIME_DLLS}")
  foreach (runtime IN LISTS runtime_dlls)
    if (EXISTS "${runtime}")
      file(COPY "${runtime}" DESTINATION "${PACKAGE_DIR}/bin")
    endif()
  endforeach()
endif()

file(WRITE "${PACKAGE_DIR}/RUN.txt"
  "Run from this directory. Give both roles --resources=resources/modules.\n"
  "Authority: bin/frontier_online_server --live --listen=41236 --expect-joins=2 --ticks=1800 --timeout=120 --resources=resources/modules\n"
  "Follower:  bin/frontier_online_server --live --join=SERVER_IPV4:41236 --ticks=1800 --timeout=120 --resources=resources/modules\n"
  "On Windows use bin/frontier_online_server.exe. All peers need identical causal resource files.\n")
