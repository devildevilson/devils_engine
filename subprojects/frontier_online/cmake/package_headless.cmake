if (NOT DEFINED BINARY OR NOT DEFINED RESOURCE_ROOT OR NOT DEFINED PACKAGE_DIR)
  message(FATAL_ERROR "headless package needs BINARY, RESOURCE_ROOT and PACKAGE_DIR")
endif()

# Rebuild the staging directory from the current source tree. A stale causal file would
# change the content root even though the executable had been rebuilt correctly.
file(REMOVE_RECURSE "${PACKAGE_DIR}")
file(MAKE_DIRECTORY "${PACKAGE_DIR}/bin" "${PACKAGE_DIR}/resources/modules")
file(COPY "${BINARY}" DESTINATION "${PACKAGE_DIR}/bin")
file(COPY "${RESOURCE_ROOT}/" DESTINATION "${PACKAGE_DIR}/resources/modules")

# Shared libraries the target machine may lack (Lua, OpenSSL, libstdc++; DLLs and the MSVC runtime
# on Windows) travel with the binary: see package_runtime.cmake.
get_filename_component(binary_name "${BINARY}" NAME)
set(PACKAGED_BINARY "${PACKAGE_DIR}/bin/${binary_name}")
set(PACKAGE_BIN "${PACKAGE_DIR}/bin")
include("${CMAKE_CURRENT_LIST_DIR}/package_runtime.cmake")

file(WRITE "${PACKAGE_DIR}/RUN.txt"
  "Headless server. Resources are found next to bin/ (resources/modules); keep the layout.\n"
  "Authority: bin/frontier_online_server --live --listen=41236 --expect-joins=2 --ticks=1800 --timeout=120\n"
  "Follower:  bin/frontier_online_server --live --join=SERVER_IPV4:41236 --ticks=1800 --timeout=120\n"
  "On Windows use bin/frontier_online_server.exe. All peers need identical causal resource files.\n"
  "For graphical clients (frontier_online --join=SERVER_IPV4:41236) add --play to the authority:\n"
  "  bin/frontier_online_server --live --play --listen=41236 --expect-joins=N --ticks=36000 --timeout=120\n"
  "With --play, N is the number of seats; a client that drops out leaves and may enter again.\n"
  "--extent=SIZE (default 64) and --world-seed=SEED set the world the clients will draw.\n")
