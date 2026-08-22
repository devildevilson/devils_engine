cmake_minimum_required(VERSION 3.25)

add_executable(PF05_scene_effects src/main.cpp src/world_ui.cpp src/world_ui.h)
target_link_libraries(PF05_scene_effects PRIVATE
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::utils
  devils_engine::options
  devils_engine::visage
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(PF05_scene_effects PRIVATE
  PF05_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources"
)
target_include_directories(PF05_scene_effects PRIVATE "${FETCHCONTENT_BASE_DIR}/nuklear-src")

set_target_properties(PF05_scene_effects PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF05_scene_effects/bin"
)

add_custom_command(TARGET PF05_scene_effects POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:PF05_scene_effects>/resources"
  VERBATIM
)
