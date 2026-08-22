cmake_minimum_required(VERSION 3.25)

add_executable(PF04_stencil_effects src/main.cpp)
target_link_libraries(PF04_stencil_effects PRIVATE
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::utils
  devils_engine::options
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(PF04_stencil_effects PRIVATE
  PF04_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources"
)

set_target_properties(PF04_stencil_effects PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF04_stencil_effects/bin"
)

add_custom_command(TARGET PF04_stencil_effects POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:PF04_stencil_effects>/resources"
  VERBATIM
)
