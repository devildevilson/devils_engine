cmake_minimum_required(VERSION 3.30)

project(
  devils_originator
  VERSION 0.1.0
  LANGUAGES CXX
)

add_library(devils_originator
  src/originator/common.cpp
  src/originator/buffer.cpp
  src/originator/tools.cpp
  src/originator/standard_tools.cpp
  src/originator/pipeline.cpp
  include/devils_engine/originator/common.h
  include/devils_engine/originator/buffer.h
  include/devils_engine/originator/tools.h
  include/devils_engine/originator/pipeline.h
)

target_include_directories(devils_originator
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/originator>"
)

target_link_libraries(devils_originator
  PUBLIC devils_engine::options devils_engine::utils
  PRIVATE tavl
)

add_library(devils_engine::originator ALIAS devils_originator)

# Скриптовый слой — ОТДЕЛЬНАЯ цель: ядро генератора не должно тянуть lua за собой, а потребителю,
# которому нужен только пайплайн на C++, незачем линковать интерпретатор.
if (MSVC)
  set(DEVILS_ORIGINATOR_LUA_LIBRARIES lua54)
else()
  find_package(PkgConfig REQUIRED)
  pkg_check_modules(originator_lua54 REQUIRED IMPORTED_TARGET lua5.4)
  set(DEVILS_ORIGINATOR_LUA_LIBRARIES PkgConfig::originator_lua54)
endif()

add_library(devils_originator_lua
  src/originator/script_host.cpp
  include/devils_engine/originator/script_host.h
)

target_include_directories(devils_originator_lua
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)

target_link_libraries(devils_originator_lua
  PUBLIC devils_engine::originator sol2::sol2 ${DEVILS_ORIGINATOR_LUA_LIBRARIES}
  PRIVATE devils_engine::options devils_engine::utils
)

if (WIN32)
  target_link_directories(devils_originator_lua PUBLIC ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54)
  target_include_directories(devils_originator_lua PRIVATE ${PROJECT_EXTERNAL_DEPENDENCY_DIR}/lua54/include)
endif()

add_library(devils_engine::originator_lua ALIAS devils_originator_lua)
