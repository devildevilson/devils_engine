cmake_minimum_required(VERSION 3.25)

project(
  devils_acumen
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/acumen/system.cpp
)

set(HDR_LIST
  include/devils_engine/acumen/common.h
  include/devils_engine/acumen/system.h
  include/devils_engine/acumen/astar.h 
)

add_library(devils_acumen ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_acumen PRIVATE devils_engine::options devils_engine::utils)
target_include_directories(devils_acumen INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_acumen PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/acumen>")

add_library(devils_engine::acumen ALIAS devils_acumen)