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
  include/devils_engine/act/script_compiler.h
  include/devils_engine/act/script_resource.h
  include/devils_engine/act/intent.h
  include/devils_engine/act/interaction.h
  include/devils_engine/act/packer.h
  include/devils_engine/act/exec_context.h
  include/devils_engine/act/function.h
  include/devils_engine/act/registry.h
)

set(SRC_LIST
  src/act/function.cpp
  src/act/registry.cpp
  src/act/script_resource.cpp
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
  PUBLIC devils_engine::options devils_engine::utils devils_engine::catalogue devils_engine::demiurg gtl devils_script
  PRIVATE tavl
)

add_library(devils_engine::act ALIAS devils_act)

# Детерминированная математика devils_script (замена init_math). Отдельная маленькая цель: её
# подключает и originator, которому не нужны catalogue/demiurg из devils_act.
add_library(devils_act_script_math src/act/script_math.cpp include/devils_engine/act/script_math.h)
target_include_directories(devils_act_script_math
  PUBLIC "$<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include/>"
)
target_link_libraries(devils_act_script_math
  PUBLIC devils_script
  PRIVATE devils_engine::options devils_engine::utils
)
add_library(devils_engine::act_script_math ALIAS devils_act_script_math)
target_link_libraries(devils_act PUBLIC devils_engine::act_script_math)
