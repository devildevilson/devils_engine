cmake_minimum_required(VERSION 3.30)

project(
  devils_utils
  VERSION 0.1.1
  LANGUAGES CXX C
)

if (WIN32) # probably can be dealed within cmake build system ???
  set(DENSITY_LIBRARY ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/density-win32/density.lib)
  set(DENSITY_LIBRARY_DIR ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/density-win32)
  set(DENSITY_INCLUDE_DIRS ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/density-win32/include)
endif()

set(SRC_LIST
  src/utils/core.cpp
  src/utils/block_allocator.cpp
  src/utils/stack_allocator.cpp
  src/utils/prng.cpp
  src/utils/fileio.cpp
  src/utils/time.cpp
  #src/utils/sha256.c
  src/utils/sha256cpp.cpp
  src/utf/utf.cpp
  src/utils/compression.cpp
  src/utils/loader.cpp
  src/utils/locale.cpp
  src/utils/patterns.cpp
)

set(HDR_LIST
  include/devils_engine/utils/block_allocator.h
  include/devils_engine/utils/stack_allocator.h
  include/devils_engine/utils/memory_pool.h
  include/devils_engine/utils/type_traits.h
  include/devils_engine/utils/core.h
  include/devils_engine/utils/string-utils.hpp
  include/devils_engine/utils/safe_handle.h
  include/devils_engine/utils/prng.h
  include/devils_engine/utils/dice.h
  
  include/devils_engine/utils/named_serializer.h
  include/devils_engine/utils/fileio.h
  include/devils_engine/utils/time-utils.hpp
  include/devils_engine/utils/sha256.h
  include/devils_engine/utf/utf.hpp
  include/devils_engine/utils/density_api.h
  include/devils_engine/utils/compression.h
  include/devils_engine/utils/load_stage.h 
  include/devils_engine/utils/loader.h 
  include/devils_engine/utils/prng_string.h 
  include/devils_engine/utils/locale-utils.hpp
  include/devils_engine/utils/shared_data_system.h 
  include/devils_engine/utils/serializable.h
  include/devils_engine/utils/event_dispatcher.h
  include/devils_engine/utils/event_consumer.h
  include/devils_engine/utils/context_stack.h
  include/devils_engine/utils/patterns.h
)

add_library(devils_mood src/mood/system.cpp include/devils_engine/mood/system.h)
target_include_directories(devils_mood INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
target_include_directories(devils_mood PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/mood>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine>")
target_link_libraries(devils_mood gtl spdlog::spdlog devils_engine::options)

add_library(devils_simul src/simul/interface.cpp include/devils_engine/simul/interface.h)
target_include_directories(devils_simul INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
target_include_directories(devils_simul PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/simul>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine>")
target_link_libraries(devils_simul spdlog::spdlog devils_engine::options)

# include/devils_engine/aesthetics/world.h include/devils_engine/aesthetics/common.h include/devils_engine/aesthetics/simple_systems.h
add_library(devils_aesthetics INTERFACE)
target_include_directories(devils_aesthetics INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
target_link_libraries(devils_aesthetics INTERFACE devils_engine::options)

# mess, why do I made this?
add_library(devils_utils ${SRC_LIST} ${HDR_LIST})

add_library(engine_c_part1 src/utils/sha256.c include/devils_engine/utils/sha256cpp.h)
target_compile_features(engine_c_part1 PRIVATE c_std_11)
target_compile_options(engine_c_part1 PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -mavx>>
)
target_compile_options(engine_c_part1 PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /O2 /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -mavx>>
)

target_link_libraries(devils_utils 
  PUBLIC devils_engine::thread devils_mood devils_simul devils_aesthetics spdlog::spdlog glaze::glaze gtl minizip ReflexLibStatic #alpaca::alpaca
  PRIVATE engine_c_part1 density ZLIB::ZLIB bcrypt ncrypt crypt32 devils_engine::options
)

target_include_directories(devils_utils INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
target_include_directories(devils_utils PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utils>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utf>")
target_include_directories(engine_c_part1 PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utils>")

target_link_directories(devils_utils PUBLIC ${DENSITY_LIBRARY_DIR})
target_include_directories(devils_utils PRIVATE ${DENSITY_INCLUDE_DIRS})

if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL GNU)
  target_link_libraries(devils_utils PUBLIC stdc++exp) # std:: print, println, ...
endif()

add_library(devils_engine::utils ALIAS devils_utils)
add_library(devils_engine::aesthetics ALIAS devils_aesthetics)
add_library(devils_engine::mood ALIAS devils_mood)
add_library(devils_engine::simul ALIAS devils_simul)
