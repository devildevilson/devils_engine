cmake_minimum_required(VERSION 3.30)

project(
  devils_utils
  VERSION 0.1.1
  LANGUAGES CXX C
)

# компрессия: zstd (уже в дереве — minizip-ng собран с MZ_ZSTD, системный libzstd).
find_package(zstd CONFIG QUIET)
if (TARGET zstd::libzstd)
  set(ZSTD_TARGET zstd::libzstd)
elseif (TARGET zstd::libzstd_shared)
  set(ZSTD_TARGET zstd::libzstd_shared)
elseif (TARGET zstd::libzstd_static)
  set(ZSTD_TARGET zstd::libzstd_static)
else()
  find_library(ZSTD_LIBRARY NAMES zstd REQUIRED)
  find_path(ZSTD_INCLUDE_DIR NAMES zstd.h REQUIRED)
  add_library(zstd_local INTERFACE)
  target_link_libraries(zstd_local INTERFACE ${ZSTD_LIBRARY})
  target_include_directories(zstd_local INTERFACE ${ZSTD_INCLUDE_DIR})
  set(ZSTD_TARGET zstd_local)
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
  include/devils_engine/utils/timeline.h
  include/devils_engine/utils/sha256.h
  include/devils_engine/utf/utf.hpp
  include/devils_engine/utils/compression.h
  include/devils_engine/utils/load_stage.h
  include/devils_engine/utils/loader.h
  include/devils_engine/utils/locale-utils.hpp
  include/devils_engine/utils/event_dispatcher.h
  include/devils_engine/utils/event_consumer.h
  include/devils_engine/utils/shared.h
  include/devils_engine/utils/kd_tree.h
 "include/devils_engine/utils/string_id.h")

# mess, why do I made this?
add_library(devils_utils ${SRC_LIST} ${HDR_LIST})

add_library(engine_c_part1 src/utils/sha256.c include/devils_engine/utils/sha256cpp.h)
target_compile_features(engine_c_part1 PRIVATE c_std_11)
target_compile_options(engine_c_part1 PRIVATE
  $<$<CONFIG:DEBUG>:$<$<C_COMPILER_ID:MSVC>:${DEVILS_ENGINE_ARCH_FLAGS_MSVC} /Wall /wd5045 /wd4820 /Zc:preprocessor>>
  $<$<CONFIG:DEBUG>:$<$<C_COMPILER_ID:GNU,Clang>:${DEVILS_ENGINE_ARCH_FLAGS_GNU} -g -Wall -Wextra>>
)
target_compile_options(engine_c_part1 PRIVATE
  $<$<CONFIG:RELEASE>:$<$<C_COMPILER_ID:MSVC>:${DEVILS_ENGINE_ARCH_FLAGS_MSVC} /Wall /O2 /Zc:preprocessor>>
  $<$<CONFIG:RELEASE>:$<$<C_COMPILER_ID:GNU,Clang>:${DEVILS_ENGINE_ARCH_FLAGS_GNU} -O3 -Wall -Wextra>>
)

target_link_libraries(devils_utils
  PUBLIC max0x7ba::atomic_queue Threads::Threads spdlog::spdlog glaze::glaze gtl #ReflexLibStatic #alpaca::alpaca
  PRIVATE engine_c_part1 ${ZSTD_TARGET} ZLIB::ZLIB devils_engine::options cpuinfo
)

if (WIN32)
  target_link_libraries(devils_utils bcrypt ncrypt crypt32)
endif()

target_include_directories(devils_utils PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>")
target_include_directories(devils_utils PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utils>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utf>" "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/thread>")
target_include_directories(engine_c_part1 PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/utils>")

if (MINGW AND CMAKE_CXX_COMPILER_ID STREQUAL GNU)
  target_link_libraries(devils_utils PUBLIC stdc++exp) # std:: print, println, ...
endif()

add_library(devils_engine::utils ALIAS devils_utils)
add_library(devils_engine::thread ALIAS devils_utils)
