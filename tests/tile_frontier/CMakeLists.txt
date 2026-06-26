cmake_minimum_required(VERSION 3.30)

project(
  tile_frontier
  VERSION 0.1.1
  LANGUAGES CXX C
)

set(PROJECT_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

set(PROJECT_EXTERNAL_DEPENDENCY_DIR ${CMAKE_CURRENT_LIST_DIR}/external)

#add_subdirectory(../)

set(HDR_LIST
  include/core/actors.h
  include/core/config.h
  include/core/messages.h
  include/core/message_dispatcher.h
  include/core/instance_layout.h
  include/core/draw_intent.h
  include/core/sound_system.h
  include/core/render_system.h
  include/core/assets_system.h
  include/core/gpu_load_context.h
  include/core/mesh_resource.h
  include/core/texture_resource.h
  include/core/texture_set.h
  include/core/tile_map.h
  include/core/tile_batch.h
  include/core/simulation.h
)

set(SRC_LIST
  main.cpp
  src/core/config.cpp
  src/core/sound_system.cpp
  src/core/render_system.cpp
  src/core/assets_system.cpp
  src/core/mesh_resource.cpp
  src/core/texture_resource.cpp
  src/core/texture_set.cpp
  src/core/tile_map.cpp
  src/core/stb_image_impl.cpp
  src/core/simulation.cpp
)

add_executable(${PROJECT_NAME} ${SRC_LIST} ${HDR_LIST})
target_link_libraries(
  ${PROJECT_NAME}
  devils_engine::devils_plane
  glm::glm
  devils_engine::options
  tavl
  Vulkan::Headers
  GPUOpen::VulkanMemoryAllocator
  VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
)
set_target_properties(${PROJECT_NAME} PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${PROJECT_OUTPUT_DIRECTORY}/bin")
target_include_directories(${PROJECT_NAME} INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
target_include_directories(${PROJECT_NAME} PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/core>")
include("${CMAKE_SOURCE_DIR}/cmake/devils_portable_runtime.cmake")
devils_bundle_runtime_dependencies(${PROJECT_NAME})

if (WIN32)
  add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
      $<TARGET_RUNTIME_DLLS:${PROJECT_NAME}>
      $<TARGET_FILE_DIR:${PROJECT_NAME}>
    COMMAND_EXPAND_LISTS
  )
endif()

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory 
    ${CMAKE_CURRENT_LIST_DIR}/resources 
    ${PROJECT_OUTPUT_DIRECTORY}/resources
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/tests/test_render_config
    ${PROJECT_OUTPUT_DIRECTORY}/render_config
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/shaders
)

add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_directory
    ${CMAKE_SOURCE_DIR}/tests/shaders
    ${PROJECT_OUTPUT_DIRECTORY}/tests/shaders
)
