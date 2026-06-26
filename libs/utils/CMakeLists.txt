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
else()
  set(DENSITY_LIBRARY ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/density/density.a)
  set(DENSITY_LIBRARY_DIR ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/density)
  set(DENSITY_INCLUDE_DIRS ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/density/include)
endif()

set(SRC_LIST
  src/utils/core.cpp
  src/utils/block_allocator.cpp
  src/utils/stack_allocator.cpp
  src/thread/pool.cpp
  src/thread/stack_pool.cpp
  src/thread/atomic_pool.cpp
  src/utils/prng.cpp
  src/utils/fileio.cpp
  src/utils/time.cpp
  #src/utils/sha256.c
  src/utils/sha256cpp.cpp
  src/utf/utf.cpp
  src/utils/compression.cpp
  src/utils/loader.cpp
  src/utils/locale.cpp
  #src/utils/patterns.cpp
)

set(HDR_LIST
  include/devils_engine/utils/block_allocator.h
  include/devils_engine/utils/stack_allocator.h
  include/devils_engine/utils/memory_pool.h
  include/devils_engine/thread/pool.h
  include/devils_engine/thread/atomic.h
  include/devils_engine/thread/lock.h
  include/devils_engine/thread/stack_pool.h
  include/devils_engine/thread/atomic_pool.h
  include/devils_engine/thread/queue1.h
  include/devils_engine/thread/spsc_queue.h
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
  #include/devils_engine/utils/patterns.h
 "include/devils_engine/utils/actor_ref.h" "include/devils_engine/utils/string_id.h")

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
  PUBLIC max0x7ba::atomic_queue Threads::Threads spdlog::spdlog glaze::glaze gtl #ReflexLibStatic #alpaca::alpaca
  PRIVATE engine_c_part1 density ZLIB::ZLIB devils_engine::options cpuinfo
)

if (WIN32)
  target_link_libraries(devils_utils bcrypt ncrypt crypt32)
endif()

target_include_directories(devils_utils PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
target_include_directories(devils_utils PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utils>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utf>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/thread>")
target_include_directories(engine_c_part1 PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utils>")

target_link_directories(devils_utils PUBLIC ${DENSITY_LIBRARY_DIR})
target_include_directories(devils_utils PRIVATE ${DENSITY_INCLUDE_DIRS})

if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL GNU)
  target_link_libraries(devils_utils PUBLIC stdc++exp) # std:: print, println, ...
endif()

add_library(devils_engine::utils ALIAS devils_utils)
add_library(devils_engine::thread ALIAS devils_utils)
