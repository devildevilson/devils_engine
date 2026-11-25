if (NOT DEFINED CLIENT_PACKAGE OR NOT DEFINED SERVER_PACKAGE OR NOT DEFINED PACKAGE_DIR)
  message(FATAL_ERROR "product package needs CLIENT_PACKAGE, SERVER_PACKAGE and PACKAGE_DIR")
endif()

# The product: one project folder with the graphical client and the headless server side by side in
# bin/, both reading the same resources/ (the server reads only resources/modules). In CI the client
# comes from the AVX build and the server from the baseline (SSE2) build; the two builds run in the
# same container image, so the shared libraries both carry are the same files. A library the server
# needs and the client lacks is added; one both carry but with different bytes is refused: the two
# executables would load one of them, built for the other.
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(COPY "${CLIENT_PACKAGE}/" DESTINATION "${PACKAGE_DIR}")
file(RENAME "${PACKAGE_DIR}/RUN.txt" "${PACKAGE_DIR}/RUN-client.txt")
file(COPY_FILE "${SERVER_PACKAGE}/RUN.txt" "${PACKAGE_DIR}/RUN-server.txt")

# Both packages must carry the same causal resources: the handshake compares them anyway, but a
# product whose own two halves disagree is a packaging defect, not a network refusal.
file(GLOB_RECURSE server_resources RELATIVE "${SERVER_PACKAGE}/resources/modules" "${SERVER_PACKAGE}/resources/modules/*")
foreach (resource IN LISTS server_resources)
  set(client_resource "${PACKAGE_DIR}/resources/modules/${resource}")
  if (NOT EXISTS "${client_resource}")
    message(FATAL_ERROR "product package: server resource '${resource}' is missing from the client package")
  endif()
  file(SHA256 "${SERVER_PACKAGE}/resources/modules/${resource}" server_hash)
  file(SHA256 "${client_resource}" client_hash)
  if (NOT server_hash STREQUAL client_hash)
    message(FATAL_ERROR "product package: resource '${resource}' differs between the client and the server package")
  endif()
endforeach()

file(GLOB server_files RELATIVE "${SERVER_PACKAGE}/bin" "${SERVER_PACKAGE}/bin/*")
foreach (name IN LISTS server_files)
  set(target "${PACKAGE_DIR}/bin/${name}")
  if (NOT EXISTS "${target}")
    file(COPY "${SERVER_PACKAGE}/bin/${name}" DESTINATION "${PACKAGE_DIR}/bin")
    continue()
  endif()
  file(SHA256 "${SERVER_PACKAGE}/bin/${name}" server_hash)
  file(SHA256 "${target}" client_hash)
  if (NOT server_hash STREQUAL client_hash)
    message(FATAL_ERROR "product package: bin/${name} differs between the client and the server package; "
                        "build both in the same environment")
  endif()
endforeach()

file(WRITE "${PACKAGE_DIR}/RUN.txt"
  "frontier_online\n"
  "  bin/frontier_online         graphical client (see RUN-client.txt)\n"
  "  bin/frontier_online_server  headless server (see RUN-server.txt)\n"
  "  resources/                  engine state (shaders, render graph) and project modules;\n"
  "                              the server reads only resources/modules\n"
  "Client and server share resources/, so their causal resources always match.\n")
