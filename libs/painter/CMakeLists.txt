cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_painter
  VERSION 0.1
  LANGUAGES CXX
)

# tiny_ktx? https://github.com/DeanoC/tiny_ktx
# ktx? https://github.com/KhronosGroup/KTX-Software

# Vulkan headers come from FetchContent and the loader is resolved at run time, so only shaderc
# is a link dependency. Windows: vcpkg. Its headers are part of painter's (and simul's) public
# surface, so on Windows the target is propagated; on Linux they live in /usr/include.
if (WIN32)
  find_package(unofficial-shaderc CONFIG REQUIRED GLOBAL)
  set(SHADERC_LIBRARY_NAME unofficial::shaderc::shaderc)
else()
  find_package(Vulkan REQUIRED)
  find_library(SHADERC_LIB NAMES shaderc_combined shaderc)
  find_package(glslang REQUIRED)
  set(SHADERC_LIBRARY_NAME ${SHADERC_LIB} glslang::glslang)
  # Debian/Ubuntu ship libshaderc_combined.a WITHOUT SPIRV-Tools (separate static archives there);
  # Arch's combined archive is self-contained and its SPIRV-Tools are shared libraries only.
  # Archives appended at the end resolve only what is still undefined, so where the combined
  # archive already has the symbols the link does not change.
  find_library(SPIRV_TOOLS_OPT_STATIC NAMES libSPIRV-Tools-opt.a)
  find_library(SPIRV_TOOLS_STATIC NAMES libSPIRV-Tools.a)
  if (SHADERC_LIB MATCHES "\\.a$" AND SPIRV_TOOLS_OPT_STATIC AND SPIRV_TOOLS_STATIC)
    list(APPEND SHADERC_LIBRARY_NAME ${SPIRV_TOOLS_OPT_STATIC} ${SPIRV_TOOLS_STATIC})
  endif()
endif()

set(SRC_LIST
  src/painter/memory_statistics.cpp
  src/painter/auxiliary.cpp
  src/painter/glsl_source_file.cpp
  src/painter/gpu_timing.cpp
  src/painter/render_config_source.cpp
  src/painter/pipeline_cache_resource.cpp
  src/painter/queue.cpp
  src/painter/makers.cpp
  src/painter/shader_compiler.cpp
  src/painter/shader_crafter.cpp
  src/painter/shader_specialization.cpp
  src/painter/system_info.cpp
  src/painter/vulkan_header.cpp
  src/painter/shader_source_file.cpp
  src/painter/common.cpp
  src/painter/compute_context.cpp
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
  include/devils_engine/painter/memory_statistics.h
  include/devils_engine/painter/atlas_layout.h
  include/devils_engine/painter/auxiliary.h
  include/devils_engine/painter/glsl_source_file.h
  include/devils_engine/painter/gpu_timing.h
  include/devils_engine/painter/render_config_source.h
  include/devils_engine/painter/makers.h
  include/devils_engine/painter/queue.h
  include/devils_engine/painter/shader_compiler.h
  include/devils_engine/painter/shader_crafter.h
  include/devils_engine/painter/shader_specialization.h
  include/devils_engine/painter/system_info.h
  include/devils_engine/painter/vulkan_header.h
  include/devils_engine/painter/vulkan_minimal.h
  include/devils_engine/painter/shader_source_file.h
  include/devils_engine/painter/common.h
  include/devils_engine/painter/common_steps.h
  include/devils_engine/painter/region_draw.h
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
target_include_directories(devils_painter PRIVATE "${stb_SOURCE_DIR}") # stb_image.h для texture_resource

if (WIN32)
  target_link_libraries(devils_painter PUBLIC ${SHADERC_LIBRARY_NAME})
endif()

add_library(devils_engine::painter ALIAS devils_painter)
