cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_thread
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/thread/pool.cpp
  src/thread/stack_pool.cpp
  src/thread/atomic_pool.cpp
)

set(HDR_LIST
  include/devils_engine/thread/pool.h
  include/devils_engine/thread/atomic.h
  include/devils_engine/thread/lock.h
  include/devils_engine/thread/stack_pool.h
  include/devils_engine/thread/atomic_pool.h
 "include/devils_engine/thread/queue1.h")

add_library(devils_thread ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_thread 
  PUBLIC max0x7ba::atomic_queue Threads::Threads
  PRIVATE devils_engine::options devils_engine::utils 
)

target_include_directories(devils_thread INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_thread PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/thread>")

add_library(devils_engine::thread ALIAS devils_thread)