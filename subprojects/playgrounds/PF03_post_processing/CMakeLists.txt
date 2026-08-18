cmake_minimum_required(VERSION 3.25)

add_executable(PF03_post_processing src/main.cpp)
target_link_libraries(PF03_post_processing PRIVATE
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::catalogue
  devils_engine::input
  devils_engine::utils
  devils_engine::options
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(PF03_post_processing PRIVATE
  PF03_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources"
)

set_target_properties(PF03_post_processing PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF03_post_processing/bin"
)

add_custom_command(TARGET PF03_post_processing POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:PF03_post_processing>/resources"
  VERBATIM
)
