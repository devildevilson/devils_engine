cmake_minimum_required(VERSION 3.30)

add_library(phy04_simulation STATIC physics_world.cpp simulation.cpp server_zones.cpp)
target_include_directories(phy04_simulation PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/../PHY03_ragdoll")
target_link_libraries(phy04_simulation PUBLIC phy03_simulation
  PRIVATE devils_engine::options devils_engine::utils)

add_executable(PHY04_reference_frames main.cpp verify.cpp)
target_link_libraries(PHY04_reference_frames PRIVATE phy04_simulation
  devils_engine::playground_cli devils_engine::options devils_engine::utils)

add_executable(PHY04_reference_frames_viewer viewer.cpp)
target_link_libraries(PHY04_reference_frames_viewer PRIVATE phy04_simulation phy03_primitive_view
  devils_engine::playground_cli devils_engine::playground_common
  devils_engine::input devils_engine::options devils_engine::utils)
set_target_properties(PHY04_reference_frames PHY04_reference_frames_viewer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PHY04_reference_frames/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  devils_add_test(NAME PHY04_reference_frames_verify COMMAND PHY04_reference_frames --verify)
  devils_add_test(NAME PHY04_server_zones_verify COMMAND PHY04_reference_frames --zones --verify)
  set_tests_properties(PHY04_reference_frames_verify PHY04_server_zones_verify
    PROPERTIES LABELS devils_engine TIMEOUT 30)
endif()
