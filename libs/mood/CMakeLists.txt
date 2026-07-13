cmake_minimum_required(VERSION 3.30)

project(
  devils_mood
  VERSION 0.1.1
  LANGUAGES CXX
)

add_library(devils_mood
  src/mood/system.cpp
  src/mood/runtime.cpp
  src/mood/registry.cpp
  include/devils_engine/mood/system.h
  include/devils_engine/mood/runtime.h
  include/devils_engine/mood/registry.h
)

target_include_directories(devils_mood
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/mood>"
)

# act PUBLIC: transition держит act::predicate_function*/effect_function*, конструктор
# system принимает const act::registry*, методы — act::exec_context (всё в публичном
# заголовке). act тянет utils+gtl PUBLIC, поэтому отдельный gtl тут больше не нужен.
target_link_libraries(devils_mood
  PUBLIC devils_engine::options devils_engine::act
  PRIVATE devils_engine::utils
)

add_library(devils_engine::mood ALIAS devils_mood)
