cmake_minimum_required(VERSION 3.30)

project(NET06_in_memory_transport VERSION 0.1.0 LANGUAGES CXX)

add_executable(NET06_in_memory_transport main.cpp)
target_link_libraries(NET06_in_memory_transport PRIVATE
  devils_engine::network
  devils_engine::utils
  devils_engine::options)

set_target_properties(NET06_in_memory_transport PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/NET06_in_memory_transport/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME NET06_in_memory_transport_verify
    COMMAND NET06_in_memory_transport --verify)
  set_tests_properties(NET06_in_memory_transport_verify PROPERTIES LABELS devils_engine)
endif()
