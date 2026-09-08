cmake_minimum_required(VERSION 3.30)

project(NETLAB01_multi_process VERSION 0.1.0 LANGUAGES CXX)

# The stand needs the real backend, so it exists only where the GNS adapter does.
if (NOT TARGET devils_engine::network_gns)
  return()
endif()

add_executable(NETLAB01_multi_process main.cpp)
target_link_libraries(NETLAB01_multi_process PRIVATE
  devils_engine::network_gns
  devils_engine::network
  devils_engine::utils
  devils_engine::options)

set_target_properties(NETLAB01_multi_process PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/NETLAB01_multi_process/bin")

# This target exists to be CARRIED to machines which cannot build, so its link
# profile is part of what it is rather than a packaging afterthought.
#
#  --as-needed        drops DT_NEEDED entries nothing references. Without it the
#                     artifact declares libraries it never calls (libzstd,
#                     reached only through an unused corner of devils_utils),
#                     and every such entry is one more thing a target machine
#                     must happen to have.
#  -static-libstdc++  the C++ runtime is the version-sensitive dependency, not
#  -static-libgcc     the presence of one: this build uses C++23 features whose
#                     libstdc++ symbols are newer than many distributions ship.
#                     Static costs about a megabyte and removes the whole axis.
#
# What deliberately stays dynamic: glibc, and OpenSSL's libcrypto, which
# GameNetworkingSockets requires for AES-GCM/SHA-256 and which every
# distribution ships. Note the SONAME is libcrypto.so.3, so a target machine
# needs OpenSSL 3.x; a 1.1-era distribution will not run this artifact.
if (UNIX AND NOT APPLE)
  target_link_options(NETLAB01_multi_process PRIVATE
    -Wl,--as-needed -static-libstdc++ -static-libgcc)
endif()

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME NETLAB01_multi_process_verify
    COMMAND NETLAB01_multi_process --verify --quiet)
  set_tests_properties(NETLAB01_multi_process_verify PROPERTIES LABELS devils_engine)
endif()
