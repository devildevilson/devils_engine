cmake_minimum_required(VERSION 3.25)

# Headless consumers must not acquire the viewer/visage dependency graph just to parse argv.
add_library(devils_playground_cli INTERFACE)
target_include_directories(devils_playground_cli INTERFACE include)
target_link_libraries(devils_playground_cli INTERFACE CLI11::CLI11)
target_compile_features(devils_playground_cli INTERFACE cxx_std_23)
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

# Экспериментальный runtime резидентности, общий для GN03/GN09. Он владеет worker lifetime,
# не алгоритмами генерации: originator core не приобретает фонового scheduler или GPU типов.
add_library(devils_playground_streaming STATIC src/chunk_streaming.cpp src/world_memory.cpp)
target_include_directories(devils_playground_streaming PUBLIC include)
target_link_libraries(devils_playground_streaming PUBLIC devils_engine::originator glm::glm PRIVATE devils_engine::utils devils_engine::options)
add_library(devils_engine::playground_streaming ALIAS devils_playground_streaming)

add_library(devils_playground_chunk_viewer STATIC src/chunk_viewer.cpp)
target_include_directories(devils_playground_chunk_viewer PUBLIC include)
target_link_libraries(devils_playground_chunk_viewer PUBLIC devils_engine::playground_streaming
  PRIVATE devils_engine::playground_common devils_engine::painter devils_engine::input
  devils_engine::visage devils_engine::utils devils_engine::options Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
target_compile_definitions(devils_playground_chunk_viewer PRIVATE
  PLAYGROUND_CHUNK_VIEWER_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources/chunk_viewer"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
add_library(devils_engine::playground_chunk_viewer ALIAS devils_playground_chunk_viewer)

if (DEVILS_ENGINE_BUILD_TESTS)
  find_package(Python3 COMPONENTS Interpreter REQUIRED)
  devils_add_test(NAME playground_cli_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_contract.py
      $<TARGET_FILE:GN01_generator_contract> $<TARGET_FILE:GN05_constraint_collapse>)
  set_tests_properties(playground_cli_contract PROPERTIES LABELS devils_engine TIMEOUT 120)
endif()

if(DEVILS_ENGINE_BUILD_TESTS)
  add_executable(playground_cli_values tests/cli_values.cpp)
  target_link_libraries(playground_cli_values PRIVATE devils_engine::playground_cli)
  devils_add_test(NAME playground_cli_values COMMAND playground_cli_values)
  set_tests_properties(playground_cli_values PROPERTIES LABELS devils_engine)
  devils_add_test(NAME playground_cli_migration_contract
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/cli_migration_contract.py
      $<TARGET_FILE:GN02_planet_generator> $<TARGET_FILE:GN03_streaming_volume>
      $<TARGET_FILE:GN04_texture_generation> $<TARGET_FILE:GN06_dungeon_layout>
      $<TARGET_FILE:GN07_location_brief> $<TARGET_FILE:GN09_next> $<TARGET_FILE:GN09_next_viewer>)
  set_tests_properties(playground_cli_migration_contract PROPERTIES LABELS devils_engine TIMEOUT 240)
endif()

if(DEVILS_ENGINE_BUILD_TESTS)
  add_executable(playground_generator_resources_contract tests/generator_resources_contract.cpp)
  target_link_libraries(playground_generator_resources_contract PRIVATE devils_engine::playground_generator)
  devils_add_test(NAME playground_generator_resources_contract
    COMMAND playground_generator_resources_contract
      "${CMAKE_CURRENT_SOURCE_DIR}/../GN02_planet_generator/resources"
      "${CMAKE_CURRENT_SOURCE_DIR}/../GN03_streaming_volume/resources"
      "${CMAKE_CURRENT_SOURCE_DIR}/../GN04_texture_generation/resources"
      "${CMAKE_CURRENT_SOURCE_DIR}/../GN06_dungeon_layout/resources")
  set_tests_properties(playground_generator_resources_contract PROPERTIES LABELS devils_engine)
endif()
