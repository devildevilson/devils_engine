cmake_minimum_required(VERSION 3.30)

project(
  devils_network
  VERSION 0.1.0
  LANGUAGES CXX
)

add_library(devils_network INTERFACE)
target_include_directories(devils_network INTERFACE
  "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)
target_link_libraries(devils_network INTERFACE devils_engine::options)

add_library(devils_engine::network ALIAS devils_network)
