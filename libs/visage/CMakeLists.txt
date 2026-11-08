cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_visage
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/visage/font.cpp
  src/visage/header.cpp
  src/visage/render_output.cpp
  src/visage/system.cpp
  src/visage/font_atlas_packer.cpp
  src/visage/font_resource.cpp
)

set(HDR_LIST
  include/devils_engine/visage/budget.h
  include/devils_engine/visage/font.h
  include/devils_engine/visage/header.h
  include/devils_engine/visage/system.h
  include/devils_engine/visage/font_atlas_packer.h
  include/devils_engine/visage/font_resource.h
)

add_library(devils_visage ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_visage
  PUBLIC devils_engine::options devils_engine::utils devils_engine::bindings_base
         devils_engine::painter devils_engine::demiurg
  PRIVATE Vulkan::Headers GPUOpen::VulkanMemoryAllocator VulkanMemoryAllocator-Hpp::VulkanMemoryAllocator-Hpp
          devils_engine::catalogue devils_engine::bindings msdfgen::msdfgen
          msdf-atlas-gen::msdf-atlas-gen glm::glm)
target_include_directories(devils_visage PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>" "$<BUILD_INTERFACE:${nuklear_SOURCE_DIR}>")
target_include_directories(devils_visage PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/visage>" "${nuklear_SOURCE_DIR}")
target_link_directories(devils_visage PRIVATE ${FREETYPE_LIBRARY_DIR})
target_include_directories(devils_visage PRIVATE ${FREETYPE_INCLUDE_DIRS})

if(MSVC)
  target_include_directories(devils_visage PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54/include)
endif()

add_library(devils_engine::visage ALIAS devils_visage)
