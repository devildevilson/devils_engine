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

target_compile_features(devils_acumen PRIVATE cxx_std_23)

target_link_libraries(devils_acumen PRIVATE devils_engine::utils)

target_compile_options(devils_acumen PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /GR- /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -fno-rtti -mavx>>
)

target_compile_options(devils_acumen PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /GR- /O2 /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -fno-rtti -mavx>>
)

target_include_directories(devils_acumen INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>") # ???

target_include_directories(devils_acumen PRIVATE 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/acumen>"
)

add_library(devils_engine::acumen ALIAS devils_acumen)