cmake_minimum_required(VERSION 3.30)

project(
  devils_aesthetics
  VERSION 0.1.1
  LANGUAGES CXX
)

add_library(devils_aesthetics
  src/aesthetics/world.cpp
  src/aesthetics/serialization.cpp
  src/aesthetics/sink.cpp
  include/devils_engine/aesthetics/common.h
  include/devils_engine/aesthetics/world.h
  include/devils_engine/aesthetics/message_buffer.h
  include/devils_engine/aesthetics/message_registry.h
  include/devils_engine/aesthetics/elect_buffer.h
  include/devils_engine/aesthetics/interaction_arena.h
  include/devils_engine/aesthetics/collect_buffer.h
  include/devils_engine/aesthetics/system_runner.h
  include/devils_engine/aesthetics/template_system.h
  include/devils_engine/aesthetics/worklist_system.h
  include/devils_engine/aesthetics/serialization.h
  include/devils_engine/aesthetics/sink.h
)

target_include_directories(devils_aesthetics
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/aesthetics>"
)

target_link_libraries(devils_aesthetics
  PUBLIC devils_engine::options devils_engine::utils gtl
)

add_library(devils_engine::aesthetics ALIAS devils_aesthetics)
