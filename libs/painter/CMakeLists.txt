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
  find_package(Vulkan REQUIRED)
  find_library(SHADERC_LIB NAMES shaderc_combined shaderc)
  find_package(glslang REQUIRED)
  set(SHADERC_LIBRARY_NAME ${SHADERC_LIB} glslang::glslang)
  set(VULKAN_LIBRARY_NAME Vulkan::Vulkan)
endif()

set(SRC_LIST
  src/painter/auxiliary.cpp
  src/painter/glsl_source_file.cpp
  src/painter/render_config_source.cpp
  src/painter/pipeline_cache_resource.cpp
  src/painter/makers.cpp
  src/painter/shader_crafter.cpp
  src/painter/system_info.cpp
  src/painter/vulkan_header.cpp
  src/painter/shader_source_file.cpp
  src/painter/common.cpp 
  src/painter/structures.cpp
  src/painter/graphics_base.cpp
  src/painter/common_steps.cpp
  src/painter/assets_base.cpp
  src/painter/gpu_texture_resource.cpp
  src/painter/texture_resource.cpp
  src/painter/mesh_resource.cpp
  src/painter/stb_image_impl.cpp
)

get_filename_component(DEVILS_ENGINE_ROOT "${CMAKE_CURRENT_LIST_DIR}/../.." ABSOLUTE)
set(DEVILS_SHARED_SHADER_HEADER "${DEVILS_ENGINE_ROOT}/libs/utils/include/devils_engine/utils/shared.h")
set(DEVILS_SHARED_SHADER_INCLUDE_TEXT "${CMAKE_CURRENT_BINARY_DIR}/generated/devils_engine/painter/bindings_shared_include_text.h")

add_custom_command(
  OUTPUT "${DEVILS_SHARED_SHADER_INCLUDE_TEXT}"
  COMMAND ${CMAKE_COMMAND}
    -DINPUT_FILE=${DEVILS_SHARED_SHADER_HEADER}
    -DOUTPUT_FILE=${DEVILS_SHARED_SHADER_INCLUDE_TEXT}
    -P "${DEVILS_ENGINE_ROOT}/cmake/devils_embed_text_header.cmake"
  DEPENDS "${DEVILS_SHARED_SHADER_HEADER}" "${DEVILS_ENGINE_ROOT}/cmake/devils_embed_text_header.cmake"
  VERBATIM
)

set(HDR_LIST
  include/devils_engine/painter/auxiliary.h
  include/devils_engine/painter/glsl_source_file.h
  include/devils_engine/painter/render_config_source.h
  include/devils_engine/painter/makers.h
  include/devils_engine/painter/shader_crafter.h
  include/devils_engine/painter/system_info.h
  include/devils_engine/painter/vulkan_header.h
  include/devils_engine/painter/vulkan_minimal.h
  include/devils_engine/painter/shader_source_file.h
  include/devils_engine/painter/common.h
  include/devils_engine/painter/common_steps.h
  include/devils_engine/painter/structures.h
  include/devils_engine/painter/graphics_base.h
  include/devils_engine/painter/assets_base.h
  "${DEVILS_SHARED_SHADER_INCLUDE_TEXT}"
 "include/devils_engine/painter/device_features.h")

add_library(devils_painter ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_painter PRIVATE devils_engine::options ${SHADERC_LIBRARY_NAME} Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp devils_engine::input devils_engine::demiurg devils_engine::utils devils_engine::catalogue sul::dynamic_bitset tavl)
target_include_directories(devils_painter INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_painter PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/painter>")
target_include_directories(devils_painter PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_BINARY_DIR}/generated>")
target_include_directories(devils_painter PRIVATE "${FETCHCONTENT_BASE_DIR}/stb-src") # stb_image.h для texture_resource

if (MSVC)
  target_link_directories(devils_painter PUBLIC ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan)
  target_include_directories(devils_painter PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan/Include)
endif()

add_library(devils_engine::painter ALIAS devils_painter)
