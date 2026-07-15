cmake_minimum_required(VERSION 3.30)

project(
  devils_act
  VERSION 0.1.0
  LANGUAGES CXX
)

set(HDR_LIST
  include/devils_engine/act/common.h
  include/devils_engine/act/value.h
  include/devils_engine/act/call_context.h
  include/devils_engine/act/execution_scratch.h
  include/devils_engine/act/stat_accessors.h
  include/devils_engine/act/intent.h
  include/devils_engine/act/interaction.h
  include/devils_engine/act/effect_sink.h
  include/devils_engine/act/exec_context.h
  include/devils_engine/act/function.h
  include/devils_engine/act/registry.h
)

set(SRC_LIST
  src/act/function.cpp
  src/act/registry.cpp
)

add_library(devils_act ${SRC_LIST} ${HDR_LIST})

target_include_directories(devils_act
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
  PRIVATE "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/devils_engine/act>"
)

# gtl PUBLIC: registry хранит gtl::flat_hash_map в публичном заголовке.
# devils_script PUBLIC: script_function<RetT> (header-only) инстанцируется у потребителей и зовёт
# devils_script::container/context — им нужны его символы и include-каталоги (тянет tavl PUBLIC).
target_link_libraries(devils_act
  PUBLIC devils_engine::options devils_engine::utils devils_engine::catalogue gtl devils_script
)

add_library(devils_engine::act ALIAS devils_act)
