cmake_minimum_required(VERSION 3.25)

project(
  devils_acumen
  VERSION 0.1
  LANGUAGES CXX
)

set(SRC_LIST
  src/acumen/system.cpp
  src/acumen/cache.cpp
  src/acumen/registry.cpp
  src/acumen/goap_resource.cpp
)

set(HDR_LIST
  include/devils_engine/acumen/common.h
  include/devils_engine/acumen/system.h
  include/devils_engine/acumen/astar.h
  include/devils_engine/acumen/cache.h
  include/devils_engine/acumen/execution_scratch.h
  include/devils_engine/acumen/registry.h
  include/devils_engine/acumen/goap_resource.h
)

add_library(devils_acumen ${SRC_LIST} ${HDR_LIST})
# act PUBLIC: state_metric/action держат act::predicate_function*/effect_function* и
# compute_state принимает act::exec_context в ПУБЛИЧНЫХ заголовках. act тянет utils+gtl PUBLIC.
target_link_libraries(devils_acumen
  PUBLIC devils_engine::act devils_engine::demiurg
  PRIVATE devils_engine::options devils_engine::utils tavl
)
target_include_directories(devils_acumen PUBLIC "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/>")
target_include_directories(devils_acumen PRIVATE "$<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/include/devils_engine/acumen>")

add_library(devils_engine::acumen ALIAS devils_acumen)
