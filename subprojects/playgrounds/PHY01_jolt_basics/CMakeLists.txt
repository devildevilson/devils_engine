cmake_minimum_required(VERSION 3.30)

project(PHY01_jolt_basics VERSION 0.1.0 LANGUAGES CXX)

add_executable(PHY01_jolt_basics main.cpp)
target_link_libraries(PHY01_jolt_basics PRIVATE
  Jolt::Jolt
  devils_engine::utils
  devils_engine::options)

set_target_properties(PHY01_jolt_basics PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PHY01_jolt_basics/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME PHY01_jolt_basics_verify
    COMMAND PHY01_jolt_basics --verify --steps=240 --bodies=48 --threads=4)
  set_tests_properties(PHY01_jolt_basics_verify PROPERTIES LABELS devils_engine)
endif()
