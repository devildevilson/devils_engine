cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_bindings
  VERSION 0.1
  LANGUAGES CXX
)

# Lua 5.4 (sol2 не умеет 5.5): одна цель на весь движок, выбор источника — в корневом CMakeLists.
set(DEVILS_LUA_LIBRARIES devils_engine::lua54)

# БАЗОВЫЕ функции lua — ОТДЕЛЬНАЯ цель, и это не косметика сборки. В них нет ничего от интерфейса
# (только utils: prng, dice, упаковка, очереди), зато нужны они и тем, у кого интерфейса нет вовсе —
# например генератору, который считает планету headless. Пока они лежали в одной цели с биндингами
# nuklear, любой потребитель `base.prng64` тянул за собой visage и nuklear целиком; заодно это
# развязывает круг «visage линкует bindings, bindings линкует visage».
add_library(devils_bindings_base
  src/bindings/env.cpp
  src/bindings/critical_error.cpp
  src/bindings/data_conversion.cpp
  include/devils_engine/bindings/data_conversion.h
  include/devils_engine/bindings/critical_error.h
  include/devils_engine/bindings/env.h
  include/devils_engine/bindings/lua_header.h
)

target_link_libraries(devils_bindings_base
  PUBLIC sol2::sol2 ${DEVILS_LUA_LIBRARIES} devils_engine::utils devils_engine::options
  PRIVATE tavl
)

target_include_directories(devils_bindings_base PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_bindings_base PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/bindings>")

add_library(devils_engine::bindings_base ALIAS devils_bindings_base)

set(SRC_LIST
  "src/bindings/nuklear_bindings.cpp"
)

set(HDR_LIST
  "include/devils_engine/bindings/nuklear_bindings.h"
)

add_library(devils_bindings ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_bindings
  PUBLIC devils_engine::bindings_base sol2::sol2 ${DEVILS_LUA_LIBRARIES}
  PRIVATE devils_engine::options devils_engine::utils devils_engine::visage glm::glm
)

target_include_directories(devils_bindings INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_bindings PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/bindings>")
target_include_directories(devils_bindings PRIVATE "${nuklear_SOURCE_DIR}")

add_library(devils_engine::bindings ALIAS devils_bindings)
