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
)

add_library(devils_thread ${SRC_LIST} ${HDR_LIST})

target_compile_features(devils_thread INTERFACE cxx_std_23)
target_compile_features(devils_thread PRIVATE cxx_std_23)

target_link_libraries(devils_thread 
  PUBLIC max0x7ba::atomic_queue Threads::Threads
  PRIVATE devils_engine::utils 
)

target_compile_options(devils_thread PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /GR- /permissive- /Zc:preprocessor>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -fno-rtti>>
)

target_compile_options(devils_thread PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /GR- /O2 /permissive- /Zc:preprocessor>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -fno-rtti>>
)

target_include_directories(devils_thread INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine>") # ???

target_include_directories(devils_thread PRIVATE 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/thread>"
)

add_library(devils_engine::thread ALIAS devils_thread)