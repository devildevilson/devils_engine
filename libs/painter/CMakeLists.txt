cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_painter
  VERSION 0.1
  LANGUAGES CXX
)

# tiny_ktx? https://github.com/DeanoC/tiny_ktx
# ktx? https://github.com/KhronosGroup/KTX-Software

if (WIN32)
  set(SHADERC_LIBRARY_NAME shaderc_shared)
  set(VULKAN_LIBRARY_NAME vulkan-1)
else()
  set(SHADERC_LIBRARY_NAME libshaderc)
  set(VULKAN_LIBRARY_NAME vulkan)
endif()

set(SRC_LIST
  src/painter/auxiliary.cpp
  src/painter/glsl_source_file.cpp
  src/painter/makers.cpp
  src/painter/shader_crafter.cpp
  #src/painter/system.cpp
  src/painter/system_info.cpp
  src/painter/vulkan_header.cpp
  #src/painter/image_pool.cpp
  #src/painter/hierarchical_image_container.cpp
  #src/painter/arbitrary_image_container.cpp
  src/painter/shader_source_file.cpp
  #src/painter/execution_pass.cpp
  src/painter/common.cpp 
  src/painter/structures.cpp
  src/painter/graphics_base.cpp
  src/painter/common_steps.cpp
  src/painter/assets_base.cpp
)

set(HDR_LIST
  include/devils_engine/painter/auxiliary.h
  include/devils_engine/painter/glsl_source_file.h
  include/devils_engine/painter/makers.h
  include/devils_engine/painter/shader_crafter.h
  #include/devils_engine/painter/system.h
  include/devils_engine/painter/system_info.h
  include/devils_engine/painter/vulkan_header.h
  include/devils_engine/painter/shader_guru.h
  include/devils_engine/painter/vulkan_minimal.h
  #include/devils_engine/painter/image_pool.h
  #include/devils_engine/painter/arbitrary_image_container.h
  #include/devils_engine/painter/hierarchical_image_container.cpp
  include/devils_engine/painter/shader_source_file.h
  #include/devils_engine/painter/execution_pass.h 
  include/devils_engine/painter/common.h
  include/devils_engine/painter/common_steps.h
  include/devils_engine/painter/structures.h
  include/devils_engine/painter/graphics_base.h
  include/devils_engine/painter/graphics_simul.h
  include/devils_engine/painter/assets_base.h
 "include/devils_engine/painter/device_features.h")

add_library(devils_painter ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_painter PRIVATE devils_engine::options ${SHADERC_LIBRARY_NAME} Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp devils_engine::input devils_engine::demiurg devils_engine::utils sul::dynamic_bitset)
target_include_directories(devils_painter INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_painter PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/painter>")

if (MSVC)
  target_link_directories(devils_painter PUBLIC ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan)
  target_include_directories(devils_painter PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan/Include)
endif()

add_library(devils_engine::painter ALIAS devils_painter)