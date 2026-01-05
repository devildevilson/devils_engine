cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_visage
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  #src/visage/draw_resource.cpp
  #src/visage/draw_stage.cpp
  src/visage/font.cpp
  src/visage/header.cpp
  src/visage/system.cpp
  src/visage/font_atlas_packer.cpp
)

set(HDR_LIST
  #include/devils_engine/visage/draw_resource.h
  #include/devils_engine/visage/draw_stage.h
  include/devils_engine/visage/font.h
  include/devils_engine/visage/header.h
  include/devils_engine/visage/system.h
  include/devils_engine/visage/font_atlas_packer.h
)

add_library(devils_visage ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_visage PRIVATE devils_engine::options Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp devils_engine::utils devils_engine::bindings devils_engine::painter msdfgen::msdfgen msdf-atlas-gen::msdf-atlas-gen glm::glm)
target_include_directories(devils_visage INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_visage PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/visage>" "${FETCHCONTENT_BASE_DIR}/nuklear-src")
target_link_directories(devils_visage PRIVATE ${FREETYPE_LIBRARY_DIR})
target_include_directories(devils_visage PRIVATE ${FREETYPE_INCLUDE_DIRS})

if(MSVC)
  target_include_directories(devils_visage PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54/include)
endif()

add_library(devils_engine::visage ALIAS devils_visage)