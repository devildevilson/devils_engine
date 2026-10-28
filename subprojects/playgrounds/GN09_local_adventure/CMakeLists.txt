cmake_minimum_required(VERSION 3.30)

project(GN09_local_adventure VERSION 0.1.0 LANGUAGES CXX)

add_executable(GN09_local_adventure src/main.cpp src/adventure.cpp src/surface.cpp src/local_structure.cpp)
target_link_libraries(GN09_local_adventure PRIVATE devils_engine::utils)
set_target_properties(GN09_local_adventure PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/GN09_local_adventure/bin")

# Та же оболочка камеры, арены и render graph, что у GN03; источник чанков здесь GN09.
set(GN03_VIEWER_SOURCE "${CMAKE_CURRENT_SOURCE_DIR}/../GN03_streaming_volume/src")
add_executable(GN09_local_adventure_viewer
  src/viewer_main.cpp src/adventure.cpp src/surface.cpp src/local_structure.cpp
  "${GN03_VIEWER_SOURCE}/viewer.cpp"
  "${GN03_VIEWER_SOURCE}/streaming.cpp"
  "${GN03_VIEWER_SOURCE}/memory.cpp")
target_include_directories(GN09_local_adventure_viewer PRIVATE "${GN03_VIEWER_SOURCE}")
target_compile_definitions(GN09_local_adventure_viewer PRIVATE
  GN03_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../GN03_streaming_volume/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources")
target_link_libraries(GN09_local_adventure_viewer PRIVATE
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
  add_test(NAME GN09_local_adventure_verify COMMAND GN09_local_adventure --verify --seeds=64)
  set_tests_properties(GN09_local_adventure_verify PROPERTIES LABELS devils_engine)
endif()
