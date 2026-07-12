cmake_minimum_required(VERSION 3.30)

project(
  devils_prefab
  VERSION 0.1.0
  LANGUAGES CXX
)

# Header-only: механизм регистрации префабов (шаблоны data/list/callback + драйвер tavl). Конкретные
# типы компонентов регистрируют внешние проекты.
add_library(devils_prefab INTERFACE)

target_include_directories(devils_prefab
  INTERFACE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

# aesthetics (world/create), act (резолв callback-имён), tavl (парс/deserialize), utils (string_hash).
target_link_libraries(devils_prefab
  INTERFACE devils_engine::options devils_engine::utils devils_engine::aesthetics devils_engine::act tavl
)

add_library(devils_engine::prefab ALIAS devils_prefab)
