cmake_minimum_required(VERSION 3.25)

add_library(devils_playground_common STATIC
  src/frame_pacer.cpp
  src/free_camera.cpp
  src/visage_overlay.cpp
)

target_include_directories(devils_playground_common PUBLIC include)
target_link_libraries(devils_playground_common PUBLIC glm::glm devils_engine::visage devils_engine::utils)
target_link_libraries(devils_playground_common PRIVATE devils_engine::bindings)
add_library(devils_engine::playground_common ALIAS devils_playground_common)
