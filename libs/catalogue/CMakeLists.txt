cmake_minimum_required(VERSION 3.25)

project(
  devils_catalogue
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/catalogue/core.cpp
  src/catalogue/demo.cpp
  src/catalogue/introspection.cpp
  src/catalogue/logging.cpp
)

set(HDR_LIST
  include/devils_engine/catalogue/core.h
  include/devils_engine/catalogue/common.h
  include/devils_engine/catalogue/rpc_function.h
  include/devils_engine/catalogue/channel_data.h
  include/devils_engine/catalogue/registry.h
  include/devils_engine/catalogue/demo.h
  include/devils_engine/catalogue/introspection.h
  include/devils_engine/catalogue/logging.h
)

add_library(devils_catalogue ${SRC_LIST} ${HDR_LIST})
target_link_libraries(devils_catalogue PUBLIC devils_engine::options devils_engine::utils)
target_include_directories(devils_catalogue PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_catalogue PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/catalogue/>")

add_library(devils_engine::catalogue ALIAS devils_catalogue)
