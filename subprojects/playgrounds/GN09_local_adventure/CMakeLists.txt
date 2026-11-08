cmake_minimum_required(VERSION 3.30)

project(GN09_local_adventure VERSION 0.1.0 LANGUAGES CXX)

add_executable(GN09_local_adventure src/main.cpp src/adventure.cpp src/world_chain.cpp src/hall.cpp src/graph_view.cpp src/surface.cpp
  src/local_structure.cpp src/station.cpp src/tunnel.cpp src/cave.cpp)
target_link_libraries(GN09_local_adventure PRIVATE
  devils_engine::originator
  devils_engine::originator_config
  devils_engine::originator_lua
  devils_engine::utils)
# Цепочка мира и правила генерации — ресурсы originator в resources/gn09.
# Читаются из дерева исходников, как у других площадок.
target_compile_definitions(GN09_local_adventure PRIVATE
  GN09_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
set_target_properties(GN09_local_adventure PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN09_local_adventure/bin")

# Та же оболочка камеры, арены и render graph, что у GN03; источник чанков здесь GN09.
add_executable(GN09_local_adventure_viewer
  src/viewer_main.cpp src/adventure.cpp src/world_chain.cpp src/hall.cpp src/surface.cpp src/local_structure.cpp src/station.cpp src/tunnel.cpp src/cave.cpp
)
target_compile_definitions(GN09_local_adventure_viewer PRIVATE
  GN09_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(GN09_local_adventure_viewer PRIVATE
  devils_engine::playground_chunk_viewer
  devils_engine::originator
  devils_engine::originator_config
  devils_engine::originator_lua
  devils_engine::originator_primitives
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::visage
  devils_engine::utils
  devils_engine::options
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
set_target_properties(GN09_local_adventure_viewer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN09_local_adventure/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  devils_add_test(NAME GN09_local_adventure_verify COMMAND GN09_local_adventure --verify --seed=0 --seeds=64)
  set_tests_properties(GN09_local_adventure_verify PROPERTIES LABELS "devils_engine;gn_campaign" TIMEOUT 7200)
  devils_add_test(NAME GN09_local_adventure_smoke COMMAND GN09_local_adventure --seed=7)
  set_tests_properties(GN09_local_adventure_smoke PROPERTIES LABELS "devils_engine;gn_smoke" TIMEOUT 180)
endif()

# Independent biome-first rewrite. Legacy executables and resources stay selectable as before.
add_executable(GN09_next src/next/main.cpp src/next/runtime.cpp
)
target_compile_definitions(GN09_next PRIVATE GN09_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(GN09_next PRIVATE devils_engine::playground_streaming devils_engine::originator devils_engine::originator_config
  devils_engine::originator_lua devils_engine::originator_primitives devils_engine::utils glm::glm)
set_target_properties(GN09_next PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN09_local_adventure/bin")

add_executable(GN09_next_viewer src/next/viewer.cpp src/next/runtime.cpp
)
target_compile_definitions(GN09_next_viewer PRIVATE
  GN09_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")
target_link_libraries(GN09_next_viewer PRIVATE devils_engine::playground_chunk_viewer devils_engine::originator devils_engine::originator_config
  devils_engine::originator_lua devils_engine::originator_primitives devils_engine::playground_common
  devils_engine::painter devils_engine::input devils_engine::visage devils_engine::utils devils_engine::options
  glm::glm Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
set_target_properties(GN09_next_viewer PROPERTIES RUNTIME_OUTPUT_DIRECTORY
  "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN09_local_adventure/bin")
if (DEVILS_ENGINE_BUILD_TESTS)
  devils_add_test(NAME GN09_next_verify COMMAND GN09_next --verify --seed=0 --seeds=20)
  set_tests_properties(GN09_next_verify PROPERTIES LABELS "devils_engine;gn_campaign" TIMEOUT 1800)
  devils_add_test(NAME GN09_next_smoke COMMAND GN09_next --verify --seed=7 --seeds=1)
  devils_add_test(NAME GN09_next_caves_smoke COMMAND GN09_next --verify-caves --seed=7 --seeds=1)
  set_tests_properties(GN09_next_smoke GN09_next_caves_smoke PROPERTIES
    LABELS "devils_engine;gn_smoke" TIMEOUT 180)
endif()

target_link_libraries(GN09_local_adventure PRIVATE devils_engine::playground_cli)
target_link_libraries(GN09_local_adventure_viewer PRIVATE devils_engine::playground_cli)
target_link_libraries(GN09_next PRIVATE devils_engine::playground_cli)
target_link_libraries(GN09_next_viewer PRIVATE devils_engine::playground_cli)
