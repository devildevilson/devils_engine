cmake_minimum_required(VERSION 3.25)

project(
  devils_catalogue
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/catalogue/core.cpp
 "src/catalogue/demo.cpp")

set(HDR_LIST
  include/devils_engine/catalogue/core.h
 "include/devils_engine/catalogue/common.h" "include/devils_engine/catalogue/rpc_function.h" "include/devils_engine/catalogue/channel_data.h" "include/devils_engine/catalogue/registry.h" "include/devils_engine/catalogue/demo.h")

add_library(devils_catalogue ${SRC_LIST} ${HDR_LIST})

target_compile_features(devils_catalogue PRIVATE cxx_std_23)

target_link_libraries(devils_catalogue PRIVATE devils_engine::utils)

target_compile_options(devils_catalogue PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /GR- /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -fno-rtti -mavx>>
)

target_compile_options(devils_catalogue PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /GR- /O2 /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -fno-rtti -mavx>>
)

target_include_directories(devils_catalogue INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>") # ???

target_include_directories(devils_catalogue PRIVATE 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/catalogue/>"
)

add_library(devils_engine::catalogue ALIAS devils_catalogue)