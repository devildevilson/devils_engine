cmake_minimum_required(VERSION 3.30)

project(GN09_local_adventure VERSION 0.1.0 LANGUAGES CXX)

# Потоковый подводный мир: биомы, сеть главных узлов, пещеры, аванпосты, поверхность чанков.
# Все правила генерации — ресурсы originator в resources/gn09_next (TAVL/Lua/DS); они читаются
# из дерева исходников, как у других площадок. C++ загружает цепочку, проверяет и показывает мир.
set(gn09_bin_dir "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN09_local_adventure/bin")
set(gn09_resource_root "${CMAKE_CURRENT_SOURCE_DIR}/resources")

# Консольная программа: проверки, отчёты, SVG, замеры потока чанков.
add_executable(GN09_next src/main.cpp src/runtime.cpp)
target_compile_definitions(GN09_next PRIVATE GN09_RESOURCE_ROOT="${gn09_resource_root}")
target_link_libraries(GN09_next PRIVATE
  devils_engine::playground_cli
  devils_engine::playground_streaming
  devils_engine::originator
  devils_engine::originator_config
  devils_engine::originator_lua
  devils_engine::originator_primitives
  devils_engine::utils
  glm::glm)
set_target_properties(GN09_next PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${gn09_bin_dir}")

# Просмотрщик: оболочка камеры, арены и render graph из playground_chunk_viewer (общая с GN03).
add_executable(GN09_next_viewer src/viewer.cpp src/runtime.cpp)
target_compile_definitions(GN09_next_viewer PRIVATE GN09_RESOURCE_ROOT="${gn09_resource_root}")
target_link_libraries(GN09_next_viewer PRIVATE
  devils_engine::playground_cli
  devils_engine::playground_chunk_viewer
  devils_engine::playground_common
  devils_engine::originator
  devils_engine::originator_config
  devils_engine::originator_lua
  devils_engine::originator_primitives
  devils_engine::painter
  devils_engine::input
  devils_engine::visage
  devils_engine::utils
  devils_engine::options
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
set_target_properties(GN09_next_viewer PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${gn09_bin_dir}")

if (DEVILS_ENGINE_BUILD_TESTS)
  devils_add_test(NAME GN09_next_verify COMMAND GN09_next --verify --seed=0 --seeds=20)
  set_tests_properties(GN09_next_verify PROPERTIES LABELS "devils_engine;gn_campaign" TIMEOUT 1800)
  devils_add_test(NAME GN09_next_smoke COMMAND GN09_next --verify --seed=7 --seeds=1)
  devils_add_test(NAME GN09_next_caves_smoke COMMAND GN09_next --verify-caves --seed=7 --seeds=1)
  set_tests_properties(GN09_next_smoke GN09_next_caves_smoke PROPERTIES
    LABELS "devils_engine;gn_smoke" TIMEOUT 180)
endif()
