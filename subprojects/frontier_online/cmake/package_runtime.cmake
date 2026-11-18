# Shared by package_headless.cmake and package_client.cmake: put next to a packaged executable
# everything it loads that the target machine is not guaranteed to have.
#
# Linux: the executable's shared-library closure (libstdc++, Lua, OpenSSL, GLFW...), found the way
#   the loader would find it; glibc itself stays the system's, so the package runs on glibc no
#   older than the build machine's. The executable carries RPATH=$ORIGIN (devils_set_local_rpath).
# Windows: every DLL vcpkg copied beside the build's executables (its applocal step follows the
#   whole dependency closure, which TARGET_RUNTIME_DLLS does not), plus the MSVC runtime.
#
# Inputs: PACKAGED_BINARY (the copy inside the package), PACKAGE_BIN, and on Windows
# BUILD_BIN_DIR and SYSTEM_RUNTIME_LIBS ('|'-separated).

if (CMAKE_HOST_WIN32)
  file(GLOB build_dlls "${BUILD_BIN_DIR}/*.dll")
  string(REPLACE "|" ";" system_runtime_libs "${SYSTEM_RUNTIME_LIBS}")
  foreach (dll IN LISTS build_dlls system_runtime_libs)
    if (EXISTS "${dll}")
      file(COPY "${dll}" DESTINATION "${PACKAGE_BIN}")
    endif()
  endforeach()
  if (system_runtime_libs STREQUAL "")
    message(WARNING "MSVC runtime DLLs were not found; the package needs the VC++ redistributable installed")
  endif()
elseif (CMAKE_HOST_UNIX AND NOT CMAKE_HOST_APPLE)
  set(DEVILS_RUNTIME_FILE "${PACKAGED_BINARY}")
  set(DEVILS_RUNTIME_OUTPUT_DIR "${PACKAGE_BIN}")
  include("${CMAKE_CURRENT_LIST_DIR}/../../../cmake/devils_copy_runtime_dependencies.cmake")
endif()
