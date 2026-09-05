cmake_minimum_required(VERSION 3.30)

project(NET07_replication_baselines VERSION 0.1.0 LANGUAGES CXX)

add_executable(NET07_replication_baselines main.cpp)
target_link_libraries(NET07_replication_baselines PRIVATE
  devils_engine::network
  devils_engine::utils
  devils_engine::options)

set_target_properties(NET07_replication_baselines PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/NET07_replication_baselines/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME NET07_replication_baselines_verify
    COMMAND NET07_replication_baselines --verify)
  set_tests_properties(NET07_replication_baselines_verify PROPERTIES LABELS devils_engine)
endif()
