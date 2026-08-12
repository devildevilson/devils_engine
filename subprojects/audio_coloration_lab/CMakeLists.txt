cmake_minimum_required(VERSION 3.30)

project(audio_coloration_lab VERSION 0.1.0 LANGUAGES CXX)

add_executable(audio_coloration_lab main.cpp)
target_link_libraries(audio_coloration_lab PRIVATE
  devils_engine::sound
  devils_engine::utils
  devils_engine::options)

set_target_properties(audio_coloration_lab PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/audio_coloration_lab/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME audio_coloration_lab_hum_dry_run
    COMMAND audio_coloration_lab --dry-run --signal hum)
  add_test(NAME audio_coloration_lab_noise_dry_run
    COMMAND audio_coloration_lab --dry-run --signal noise)
  set_tests_properties(
    audio_coloration_lab_hum_dry_run
    audio_coloration_lab_noise_dry_run
    PROPERTIES LABELS devils_engine)
endif()
