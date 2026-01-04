cmake_minimum_required(VERSION 3.25)

#include(FetchContent)

project(
  devils_demiurg
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/demiurg/resource_system.cpp
  src/demiurg/folder_module.cpp 
  src/demiurg/modules_listing.cpp
  src/demiurg/zip_module.cpp
  src/demiurg/module_system.cpp
)

set(HDR_LIST
  include/devils_engine/demiurg/resource_base.h
  include/devils_engine/demiurg/resource_system.h
  include/devils_engine/demiurg/module_interface.h 
  include/devils_engine/demiurg/folder_module.h 
  include/devils_engine/demiurg/modules_listing.h
  include/devils_engine/demiurg/settings.h 
  include/devils_engine/demiurg/zip_module.h
  include/devils_engine/demiurg/module_system.h
  "include/devils_engine/demiurg/resource_loader.h")

add_library(devils_demiurg ${SRC_LIST} ${HDR_LIST})

target_compile_features(devils_demiurg PRIVATE cxx_std_23)

target_link_libraries(devils_demiurg PRIVATE devils_engine::utils)

target_compile_options(devils_demiurg PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /GR- /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -fno-rtti -mavx>>
)

target_compile_options(devils_demiurg PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /GR- /O2 /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -fno-rtti -mavx>>
)

target_include_directories(devils_demiurg INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>") # ???

target_include_directories(devils_demiurg PRIVATE 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/demiurg>"
)

add_library(devils_engine::demiurg ALIAS devils_demiurg)