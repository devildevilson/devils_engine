cmake_minimum_required(VERSION 3.25)

add_executable(PF06_submarine_light_room src/main.cpp)
target_link_libraries(PF06_submarine_light_room PRIVATE
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
target_compile_definitions(PF06_submarine_light_room PRIVATE
  PF06_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources"
)

set_target_properties(PF06_submarine_light_room PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF06_submarine_light_room/bin"
)

add_custom_command(TARGET PF06_submarine_light_room POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:PF06_submarine_light_room>/resources"
  VERBATIM
)
