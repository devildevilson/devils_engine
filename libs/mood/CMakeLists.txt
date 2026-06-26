cmake_minimum_required(VERSION 3.30)

project(
  devils_mood
  VERSION 0.1.1
  LANGUAGES CXX
)

add_library(devils_mood
  src/mood/system.cpp
  include/devils_engine/mood/system.h
)

target_include_directories(devils_mood
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/mood>"
)

target_link_libraries(devils_mood
  PUBLIC devils_engine::options gtl
  PRIVATE devils_engine::utils
)

add_library(devils_engine::mood ALIAS devils_mood)
