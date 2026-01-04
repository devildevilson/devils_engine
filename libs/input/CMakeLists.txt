cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_input
  VERSION 0.1
  LANGUAGES CXX C
)

if (MSVC_TOOLSET_VERSION EQUAL 120)
  set(GLFW3_FOLDER_NAME lib-vc2013)
elseif (MSVC_TOOLSET_VERSION EQUAL 140)
  set(GLFW3_FOLDER_NAME lib-vc2015)
elseif (MSVC_TOOLSET_VERSION EQUAL 141)
  set(GLFW3_FOLDER_NAME lib-vc2017)
elseif (MSVC_TOOLSET_VERSION EQUAL 142)
  set(GLFW3_FOLDER_NAME lib-vc2019)
elseif (MSVC_TOOLSET_VERSION EQUAL 143)
  set(GLFW3_FOLDER_NAME lib-vc2022)
elseif (MINGW)
  set(GLFW3_FOLDER_NAME lib-mingw-w64)
endif()

set(SRC_LIST
  src/input/core.cpp
  src/input/events.cpp
  src/input/key_names.cpp
)

set(HDR_LIST
  include/devils_engine/input/core.h
  include/devils_engine/input/events.h
  include/devils_engine/input/key_names.h
)

add_library(devils_input ${SRC_LIST} ${HDR_LIST})

target_compile_features(devils_input PRIVATE cxx_std_23)

target_link_libraries(devils_input PRIVATE devils_engine::utils ${GLFW3_LIBRARY_NAME} Vulkan::Headers)

target_compile_options(devils_input PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /wd4514 /wd4191 /GR- /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -fno-rtti -mavx>>
)

target_compile_options(devils_input PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /GR- /O2 /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -fno-rtti -mavx>>
)

target_include_directories(devils_input INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>") # ???

target_include_directories(devils_input PRIVATE 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/input>"
)

target_include_directories(devils_input SYSTEM PRIVATE
  "${FETCHCONTENT_BASE_DIR}/glfw3-src/include"
)

target_link_directories(devils_input PUBLIC
  "${FETCHCONTENT_BASE_DIR}/glfw3-src/${GLFW3_FOLDER_NAME}"
)

#if (MSVC)
#  target_include_directories(devils_input PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/vulkan/Include)
#endif()

add_library(devils_engine::input ALIAS devils_input)