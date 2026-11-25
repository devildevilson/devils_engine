if (NOT DEFINED BINARY OR NOT DEFINED BUILD_OUTPUT OR NOT DEFINED PACKAGE_DIR)
  message(FATAL_ERROR "client package needs BINARY, BUILD_OUTPUT and PACKAGE_DIR")
endif()

# The graphical client finds everything relative to its executable (utils::project_folder() =
# the directory above bin/), so the package repeats the build's layout: bin/ and resources/
# (resources/engine — the engine's immutable state: shaders, render graph; resources/modules — the
# project's resources). Files the client writes at run time (cache/, settings.tavl, debug.tavl,
# key_bingings.tavl) belong to the player's machine and are not copied.
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}/bin")
file(COPY "${BINARY}" DESTINATION "${PACKAGE_DIR}/bin")
file(COPY "${BUILD_OUTPUT}/resources" DESTINATION "${PACKAGE_DIR}")
file(REMOVE "${PACKAGE_DIR}/resources/.frontier_online_synced")

get_filename_component(binary_name "${BINARY}" NAME)
set(PACKAGED_BINARY "${PACKAGE_DIR}/bin/${binary_name}")
set(PACKAGE_BIN "${PACKAGE_DIR}/bin")
include("${CMAKE_CURRENT_LIST_DIR}/package_runtime.cmake")

file(WRITE "${PACKAGE_DIR}/RUN.txt"
  "Graphical client. Without arguments: local simulation in a window.\n"
  "With --join=SERVER_IPV4:PORT: shows the live session of a headless authority started as\n"
  "  frontier_online_server --live --play --listen=PORT --expect-joins=N --ticks=36000\n"
  "(click places food; it appears after a round trip). The world size and seed come from the\n"
  "authority (--extent, --world-seed). After a disconnect the client enters the session again.\n"
  "Linux:   bin/frontier_online [--join=HOST:PORT]\n"
  "Windows: bin\\frontier_online.exe [--join=HOST:PORT]\n"
  "Needs a Vulkan 1.0 driver. Keep the directory layout: the client looks for resources/ next to bin/.\n"
  "The directory must be writable: the client creates cache/ and its settings files next to bin/.\n")
