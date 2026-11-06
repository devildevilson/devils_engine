cmake_minimum_required(VERSION 3.25)

# Headless consumers must not acquire the viewer/visage dependency graph just to parse argv.
add_library(devils_playground_cli INTERFACE)
target_include_directories(devils_playground_cli INTERFACE include)
target_link_libraries(devils_playground_cli INTERFACE CLI11::CLI11)
add_library(devils_engine::playground_cli ALIAS devils_playground_cli)

add_library(devils_playground_generator INTERFACE)
target_include_directories(devils_playground_generator INTERFACE include)
target_link_libraries(devils_playground_generator INTERFACE devils_engine::originator_config)
add_library(devils_engine::playground_generator ALIAS devils_playground_generator)

add_library(devils_playground_common STATIC
  src/frame_pacer.cpp
  src/free_camera.cpp
  src/visage_overlay.cpp
)

target_include_directories(devils_playground_common PUBLIC include)
target_link_libraries(devils_playground_common PUBLIC glm::glm devils_engine::visage devils_engine::utils)
target_link_libraries(devils_playground_common PRIVATE devils_engine::bindings)
add_library(devils_engine::playground_common ALIAS devils_playground_common)

if (DEVILS_ENGINE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  add_test(NAME playground_cli_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_contract.py
      $<TARGET_FILE:GN01_generator_contract> $<TARGET_FILE:GN05_constraint_collapse>)
  set_tests_properties(playground_cli_contract PROPERTIES LABELS devils_engine TIMEOUT 120)
endif()
