cmake_minimum_required(VERSION 3.25)

project(
  devils_sound
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/sound/decoder.cpp
  src/sound/mp3_decoder.cpp
  src/sound/flac_decoder.cpp
  src/sound/wav_decoder.cpp
  src/sound/ogg_decoder.cpp
  src/sound/opus_decoder.cpp
  src/sound/pcm_decoder.cpp
  src/sound/system.cpp
  src/sound/resource.cpp
  src/sound/sound_resource.cpp
)

set(HDR_LIST
  include/devils_engine/sound/mp3_decoder.h
  include/devils_engine/sound/flac_decoder.h
  include/devils_engine/sound/wav_decoder.h
  include/devils_engine/sound/ogg_decoder.h
  include/devils_engine/sound/opus_decoder.h
  include/devils_engine/sound/pcm_decoder.h
  include/devils_engine/sound/decoder.h
  include/devils_engine/sound/system.h
  include/devils_engine/sound/resource.h
  include/devils_engine/sound/common.h
  include/devils_engine/sound/sound_resource.h
)

add_library(devils_sound ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_sound PRIVATE
  devils_engine::options
  devils_engine::utils
  devils_engine::catalogue
  devils_engine::demiurg
  miniaudio
  OpusFile::opusfile)
target_include_directories(devils_sound INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_sound PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/sound>")
target_include_directories(devils_sound PRIVATE "${FETCHCONTENT_BASE_DIR}/stb-src" "${FETCHCONTENT_BASE_DIR}/dr_libs-src")

add_library(devils_engine::sound ALIAS devils_sound)
