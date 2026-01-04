cmake_minimum_required(VERSION 3.25)

include(FetchContent)

# тут еще нужен openal-soft, как его сюда добавить без компиляции?

project(
  devils_sound
  VERSION 0.1
  LANGUAGES CXX
)

if (WIN32) 
  set(OPENAL_LIBRARY ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/openal/OpenAL32.lib)
  set(OPENAL_LIBRARY_DIR ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/openal)
  set(OPENAL_INCLUDE_DIRS ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/openal/include)
endif()

if (WIN32)
  set(OPENAL_LIBRARY_NAME OpenAL32)
else()
  set(OPENAL_LIBRARY_NAME openal)
endif()

set(SRC_LIST
  src/sound/al_helper.cpp
  src/sound/mp3_decoder.cpp
  src/sound/flac_decoder.cpp
  src/sound/wav_decoder.cpp
  src/sound/ogg_decoder.cpp
  src/sound/pcm_decoder.cpp
  src/sound/system.cpp
  src/sound/resource.cpp 
  src/sound/basic_sources.cpp
)

set(HDR_LIST
  include/devils_engine/sound/al_helper.h
  include/devils_engine/sound/mp3_decoder.h
  include/devils_engine/sound/flac_decoder.h
  include/devils_engine/sound/wav_decoder.h
  include/devils_engine/sound/ogg_decoder.h
  include/devils_engine/sound/pcm_decoder.h
  include/devils_engine/sound/decoder.h
  include/devils_engine/sound/system.h
  include/devils_engine/sound/virtual_source.h 
  include/devils_engine/sound/resource.h 
  include/devils_engine/sound/basic_sources.h
)

add_library(devils_sound ${SRC_LIST} ${HDR_LIST})

target_compile_features(devils_sound PRIVATE cxx_std_23)

target_link_libraries(devils_sound PRIVATE devils_engine::utils glm::glm ${OPENAL_LIBRARY_NAME}) # openal

target_compile_options(devils_sound PRIVATE 
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /wd5045 /wd4820 /GR- /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:DEBUG>:$<$<CXX_COMPILER_ID:GNU,Clang>:-g -Wall -Wextra -fno-rtti -mavx>>
)

target_compile_options(devils_sound PRIVATE 
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:MSVC>:/Wall /GR- /O2 /permissive- /Zc:preprocessor /arch:AVX>>
  $<$<CONFIG:RELEASE>:$<$<CXX_COMPILER_ID:GNU,Clang>:-O3 -Wall -Wextra -fno-rtti -mavx>>
)

target_include_directories(devils_sound INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>") # ???

target_include_directories(devils_sound PRIVATE 
  "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/sound>"
)

target_link_directories(devils_sound PUBLIC ${OPENAL_LIBRARY_DIR})
target_include_directories(devils_sound PRIVATE ${OPENAL_INCLUDE_DIRS})

target_include_directories(devils_sound PRIVATE "${FETCHCONTENT_BASE_DIR}/stb-src" "${FETCHCONTENT_BASE_DIR}/dr_libs-src")

add_library(devils_engine::sound ALIAS devils_sound)