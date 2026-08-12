cmake_minimum_required(VERSION 3.30)

project(audio_spatial_lab VERSION 0.1.0 LANGUAGES CXX)

add_executable(audio_spatial_lab main.cpp)
target_link_libraries(audio_spatial_lab PRIVATE
  devils_engine::sound
  devils_engine::utils
  devils_engine::options)

if (WIN32)
  target_link_libraries(audio_spatial_lab PRIVATE
    "${PROJECT_EXTERNAL_DEPENDENCY_DIR}/openal/OpenAL32.lib")
  target_include_directories(audio_spatial_lab PRIVATE
    "${PROJECT_EXTERNAL_DEPENDENCY_DIR}/openal/include")
else()
  find_package(OpenAL REQUIRED)
  target_link_libraries(audio_spatial_lab PRIVATE OpenAL::OpenAL)
endif()

set_target_properties(audio_spatial_lab PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/subprojects/audio_spatial_lab/bin")

if (DEVILS_ENGINE_BUILD_TESTS)
  add_test(NAME audio_spatial_lab_dry_run
    COMMAND audio_spatial_lab --dry-run)
  set_tests_properties(audio_spatial_lab_dry_run PROPERTIES LABELS devils_engine)
endif()
