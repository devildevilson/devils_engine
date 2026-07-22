cmake_minimum_required(VERSION 3.30)

project(
  devils_resolve
  VERSION 0.1.0
  LANGUAGES CXX
)

add_library(devils_resolve INTERFACE)
target_include_directories(devils_resolve INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)
target_link_libraries(devils_resolve INTERFACE
  devils_engine::options
  devils_engine::utils
)

add_library(devils_engine::resolve ALIAS devils_resolve)
