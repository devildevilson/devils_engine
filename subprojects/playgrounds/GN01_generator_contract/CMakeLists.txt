cmake_minimum_required(VERSION 3.30)

project(GN01_generator_contract VERSION 0.1.0 LANGUAGES CXX)

add_executable(GN01_generator_contract main.cpp)

target_compile_definitions(GN01_generator_contract PRIVATE
  GN01_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")

target_link_libraries(GN01_generator_contract PRIVATE
  devils_engine::originator
  devils_engine::originator_lua
  devils_engine::originator_primitives
  devils_engine::utils
  devils_engine::options)

set_target_properties(GN01_generator_contract PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN01_generator_contract/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME GN01_generator_contract_verify
    COMMAND GN01_generator_contract --verify --size=256)
  set_tests_properties(GN01_generator_contract_verify PROPERTIES LABELS devils_engine)
endif()
