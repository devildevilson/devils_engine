cmake_minimum_required(VERSION 3.25)

add_executable(PF10_functional_planet
  src/main.cpp
  src/planet.cpp src/planet.h
  src/viewer.cpp src/viewer.h
)
target_link_libraries(PF10_functional_planet PRIVATE
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::utils
  devils_engine::visage
  glm::glm
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(PF10_functional_planet PRIVATE
  PF10_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources"
)

set_target_properties(PF10_functional_planet PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF10_functional_planet/bin"
)

add_custom_command(TARGET PF10_functional_planet POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E make_directory "$<TARGET_FILE_DIR:PF10_functional_planet>/../cache"
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources" "$<TARGET_FILE_DIR:PF10_functional_planet>/resources"
  VERBATIM
)
