cmake_minimum_required(VERSION 3.25)

include(FetchContent)

project(
  devils_bindings
  VERSION 0.1
  LANGUAGES CXX
)

if (MSVC)
  # windows: prebuilt lua54 (см. WIN32-блок ниже)
  set(DEVILS_LUA_LIBRARIES lua54)
else()
  # sol2 больше не поддерживается и не умеет lua5.5. find_package(Lua) на этой системе
  # цепляет несовместимую liblua5.5 при заголовках 5.4, поэтому берем строго lua5.4 через pkg-config.
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(lua54 REQUIRED IMPORTED_TARGET lua5.4)
  set(DEVILS_LUA_LIBRARIES PkgConfig::lua54)
endif()

# БАЗОВЫЕ функции lua — ОТДЕЛЬНАЯ цель, и это не косметика сборки. В них нет ничего от интерфейса
# (только utils: prng, dice, упаковка, очереди), зато нужны они и тем, у кого интерфейса нет вовсе —
# например генератору, который считает планету headless. Пока они лежали в одной цели с биндингами
# nuklear, любой потребитель `base.prng64` тянул за собой visage и nuklear целиком; заодно это
# развязывает круг «visage линкует bindings, bindings линкует visage».
add_library(devils_bindings_base
  src/bindings/env.cpp
  include/devils_engine/bindings/env.h
  include/devils_engine/bindings/lua_header.h
)

target_link_libraries(devils_bindings_base
  PUBLIC sol2::sol2 ${DEVILS_LUA_LIBRARIES}
  PRIVATE devils_engine::options devils_engine::utils
)

target_include_directories(devils_bindings_base INTERFACE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_bindings_base PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/bindings>")

if (WIN32)
  target_link_directories(devils_bindings_base PUBLIC ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54)
  target_include_directories(devils_bindings_base PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54/include)
endif()

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
target_include_directories(devils_bindings PRIVATE "${FETCHCONTENT_BASE_DIR}/nuklear-src")

if (WIN32)
  target_link_directories(devils_bindings PUBLIC ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54)
  target_include_directories(devils_bindings PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54/include)
endif()

add_library(devils_engine::bindings ALIAS devils_bindings)
