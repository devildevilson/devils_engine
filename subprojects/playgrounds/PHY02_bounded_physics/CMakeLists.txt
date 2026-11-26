cmake_minimum_required(VERSION 3.30)

add_executable(PHY02_bounded_physics main.cpp memory.cpp)
target_link_libraries(PHY02_bounded_physics PRIVATE
  Jolt::Jolt
  devils_engine::thread
  devils_engine::options
  devils_engine::playground_cli)

set_target_properties(PHY02_bounded_physics PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PHY02_bounded_physics/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  devils_add_test(NAME PHY02_bounded_physics_verify
    COMMAND PHY02_bounded_physics --verify)
  devils_add_test(NAME PHY02_bounded_physics_scratch_limit
    COMMAND ${CMAKE_COMMAND} -DPROBE=$<TARGET_FILE:PHY02_bounded_physics>
    -P ${CMAKE_CURRENT_SOURCE_DIR}/scratch_limit.cmake)
  set_tests_properties(PHY02_bounded_physics_verify PHY02_bounded_physics_scratch_limit
    PROPERTIES LABELS devils_engine TIMEOUT 45)
endif()
