cmake_minimum_required(VERSION 3.30)

project(
  devils_prefab
  VERSION 0.1.0
  LANGUAGES CXX
)

add_library(devils_prefab
  src/prefab/resource.cpp
  include/devils_engine/prefab/prefab_registry.h
  include/devils_engine/prefab/resource.h
)

target_include_directories(devils_prefab
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

# aesthetics (world/create), act (резолв callback-имён), tavl (парс/deserialize), utils (string_hash).
target_link_libraries(devils_prefab
  PUBLIC devils_engine::options devils_engine::utils devils_engine::aesthetics devils_engine::act devils_engine::demiurg tavl
)

add_library(devils_engine::prefab ALIAS devils_prefab)
