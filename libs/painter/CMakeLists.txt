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
  src/painter/system.cpp
  src/painter/system_info.cpp
  src/painter/vulkan_header.cpp
  src/painter/attachments_container.cpp 
  src/painter/container.cpp 
  src/painter/queue_stages.cpp
  src/painter/render_pass_stages.cpp
  src/painter/common_stages.cpp
  src/painter/layouting.cpp
  src/painter/image_pool.cpp
  src/painter/hierarchical_image_container.cpp
  src/painter/pipelines_resources.cpp
  src/painter/arbitrary_image_container.cpp
  src/painter/render_pass_resources.cpp
  src/painter/framebuffer_resources.cpp
  src/painter/swapchain_resources.cpp
  src/painter/pipelines_config_static_container.cpp
  src/painter/buffer_resources.cpp
  src/painter/shader_source_file.cpp
)

set(HDR_LIST
  include/devils_engine/painter/auxiliary.h
  include/devils_engine/painter/glsl_source_file.h
  include/devils_engine/painter/makers.h
  include/devils_engine/painter/shader_crafter.h
  include/devils_engine/painter/system.h
  include/devils_engine/painter/system_info.h
  include/devils_engine/painter/vulkan_header.h
  include/devils_engine/painter/common.h
  include/devils_engine/painter/shader_guru.h
  include/devils_engine/painter/primitives.h
  include/devils_engine/painter/stage_buffer.h
  include/devils_engine/painter/vulkan_minimal.h
  include/devils_engine/painter/attachments_container.h
  include/devils_engine/painter/container.h
  include/devils_engine/painter/queue_stages.h 
  include/devils_engine/painter/render_pass_stages.h
  include/devils_engine/painter/common_stages.h
  include/devils_engine/painter/layouting.h
  include/devils_engine/painter/image_pool.h
  include/devils_engine/painter/pipelines_resources.h
  include/devils_engine/painter/pipeline_create_config.h
  include/devils_engine/painter/pipelines_config_static_container.h
  include/devils_engine/painter/arbitrary_image_container.h
  include/devils_engine/painter/render_pass_resources.h
  include/devils_engine/painter/framebuffer_resources.h
  include/devils_engine/painter/swapchain_resources.h
  include/devils_engine/painter/buffer_resources.h
  include/devils_engine/painter/shader_source_file.h
)

add_library(devils_painter ${SRC_LIST} ${HDR_LIST})

target_compile_features(devils_painter PRIVATE cxx_std_23)

#${VULKAN_LIBRARY_NAME} # dynamic loading?
target_link_libraries(devils_painter PRIVATE ${SHADERC_LIBRARY_NAME} Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp devils_engine::input devils_engine::demiurg devils_engine::utils sul::dynamic_bitset)

target_compile_options(devils_painter PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /wd4514 /wd4191 /GR- /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -fno-rtti -mavx>>
)

target_compile_options(devils_painter PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /GR- /O2 /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -fno-rtti -mavx>>
)

target_include_directories(devils_painter INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine>") # ???

target_include_directories(devils_painter PRIVATE 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/painter>"
)

if (MSVC)
  target_link_directories(devils_painter PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan)
  target_include_directories(devils_painter PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan/Include)
endif()

add_library(devils_engine::painter ALIAS devils_painter)