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

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME NETLAB01_multi_process_verify
    COMMAND NETLAB01_multi_process --verify --quiet)
  set_tests_properties(NETLAB01_multi_process_verify PROPERTIES LABELS devils_engine)
endif()
