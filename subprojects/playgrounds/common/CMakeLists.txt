cmake_minimum_required(VERSION 3.25)

add_library(devils_playground_common STATIC
  src/free_camera.cpp
)

target_include_directories(devils_playground_common PUBLIC include)
target_link_libraries(devils_playground_common PUBLIC glm::glm)
add_library(devils_engine::playground_common ALIAS devils_playground_common)
