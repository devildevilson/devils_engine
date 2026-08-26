cmake_minimum_required(VERSION 3.25)

# Небесная механика (срез 1) остаётся headless и не зависит от Vulkan; графический слой среза 2
# живёт в отдельных файлах, поэтому командные режимы можно запускать на машине без дисплея.
add_executable(PF07_party_environment
  src/main.cpp
  src/celestial.cpp src/celestial.h
  src/survey.cpp src/survey.h
  src/sky_frame.cpp src/sky_frame.h
  src/sky_view.cpp src/sky_view.h
)
target_link_libraries(PF07_party_environment PRIVATE
  devils_engine::playground_common
  devils_engine::painter
  devils_engine::input
  devils_engine::utils
  devils_engine::options
  devils_engine::visage
  glm::glm
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
target_compile_definitions(PF07_party_environment PRIVATE
  PF07_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/resources"
  PLAYGROUND_COMMON_RESOURCE_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/../common/resources"
)

set_target_properties(PF07_party_environment PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/playgrounds/PF07_party_environment/bin"
)

add_custom_command(TARGET PF07_party_environment POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    "${CMAKE_CURRENT_SOURCE_DIR}/resources"
    "$<TARGET_FILE_DIR:PF07_party_environment>/resources"
  VERBATIM
)
