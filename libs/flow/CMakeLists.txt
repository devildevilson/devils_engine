cmake_minimum_required(VERSION 3.30)

project(
  devils_flow
  VERSION 0.1.0
  LANGUAGES CXX
)

add_library(devils_flow
  src/flow/system.cpp
  src/flow/animation_resource.cpp
  include/devils_engine/flow/system.h
  include/devils_engine/flow/animation_resource.h
)

target_include_directories(devils_flow
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/flow>"
)

target_link_libraries(devils_flow
  PUBLIC devils_engine::options devils_engine::demiurg devils_engine::utils
  PRIVATE tavl
)

add_library(devils_engine::flow ALIAS devils_flow)
