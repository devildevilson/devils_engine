if (NOT DEFINED BINARY OR NOT DEFINED BUILD_OUTPUT OR NOT DEFINED PACKAGE_DIR)
  message(FATAL_ERROR "client package needs BINARY, BUILD_OUTPUT and PACKAGE_DIR")
endif()

# The graphical client finds everything relative to its executable (utils::project_folder() =
# the directory above bin/), so the package repeats the build's layout: bin/, resources/,
# shaders/, tests/shaders/. Files the client writes at run time (cache/, settings.tavl,
# key_bingings.tavl) belong to the player's machine and are not copied.
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}/bin" "${PACKAGE_DIR}/tests")
file(COPY "${BINARY}" DESTINATION "${PACKAGE_DIR}/bin")
file(COPY "${BUILD_OUTPUT}/resources" "${BUILD_OUTPUT}/shaders" DESTINATION "${PACKAGE_DIR}")
file(COPY "${BUILD_OUTPUT}/tests/shaders" DESTINATION "${PACKAGE_DIR}/tests")

get_filename_component(binary_name "${BINARY}" NAME)
set(PACKAGED_BINARY "${PACKAGE_DIR}/bin/${binary_name}")
set(PACKAGE_BIN "${PACKAGE_DIR}/bin")
include("${CMAKE_CURRENT_LIST_DIR}/package_runtime.cmake")

file(WRITE "${PACKAGE_DIR}/RUN.txt"
  "Graphical client: local simulation in a window. It does not connect to a server yet;\n"
  "the networked roles are the headless package.\n"
  "Linux:   bin/frontier_online\n"
  "Windows: bin\\frontier_online.exe\n"
  "Needs a Vulkan 1.0 driver. Keep the directory layout: the client looks for resources/ next to bin/.\n"
  "The directory must be writable: the client creates cache/ and its settings files next to bin/.\n")
