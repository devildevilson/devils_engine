cmake_minimum_required(VERSION 3.30)

add_library(phy03_simulation STATIC simulation.cpp recipe.cpp replay.cpp)
target_link_libraries(phy03_simulation PUBLIC Jolt::Jolt devils_engine::aesthetics
  PRIVATE devils_engine::options devils_engine::utils)

add_executable(PHY03_ragdoll main.cpp)
target_link_libraries(PHY03_ragdoll PRIVATE phy03_simulation
  devils_engine::playground_cli devils_engine::options devils_engine::utils)
set_target_properties(PHY03_ragdoll PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PHY03_ragdoll/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  devils_add_test(NAME PHY03_ragdoll_verify COMMAND PHY03_ragdoll --verify)
  set_tests_properties(PHY03_ragdoll_verify PROPERTIES LABELS devils_engine TIMEOUT 30)
endif()

add_library(phy03_primitive_view STATIC primitive_view.cpp)
target_include_directories(phy03_primitive_view PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")
target_link_libraries(phy03_primitive_view PUBLIC phy03_simulation glm::glm
  PRIVATE devils_engine::playground_common devils_engine::input devils_engine::painter
  devils_engine::options devils_engine::utils
  Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp)
target_compile_definitions(phy03_primitive_view PRIVATE
  PHY03_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources")

add_executable(PHY03_ragdoll_viewer viewer.cpp)
target_link_libraries(PHY03_ragdoll_viewer PRIVATE phy03_primitive_view
  devils_engine::playground_cli devils_engine::playground_common
  devils_engine::input devils_engine::options devils_engine::utils)
set_target_properties(PHY03_ragdoll_viewer PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PHY03_ragdoll/bin")
