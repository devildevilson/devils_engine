# Регрессия на дефект №1 (полоса scratch была привязана к потоку ОС).
#
# Запускает авторитет дважды с ОДНИМИ И ТЕМИ ЖЕ аргументами и требует одинаковый корень.
# Это самая слабая из мыслимых проверок детерминизма — «один и тот же мир на одной и той же
# машине с одним и тем же числом потоков» — и ровно она падала до исправления: при четырёх
# потоках каждый прогон давал новый корень.
#
# Мир берётся С ДОБЫЧЕЙ намеренно: именно она вводит в игру путь, на котором дефект виден.
# Прогон без prey проходил и до исправления, поэтому проверка на нём ничего бы не сторожила.

if (NOT DEFINED FRONTIER_SERVER)
  message(FATAL_ERROR "check_reproducible: FRONTIER_SERVER is not set")
endif()

set(ARGS --ticks=200 --actors=256 --workers=4 --prefabs=prey,prey,prey,actor --verify)

function(run_once output_variable)
  execute_process(
    COMMAND ${FRONTIER_SERVER} ${ARGS}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE code)
  if (NOT code EQUAL 0)
    message(FATAL_ERROR "authority exited with ${code}\n${out}\n${err}")
  endif()
  if (NOT out MATCHES "state\\.root = ([0-9]+)")
    message(FATAL_ERROR "authority printed no state.root\n${out}")
  endif()
  set(${output_variable} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

run_once(first)
run_once(second)

if (NOT first STREQUAL second)
  message(FATAL_ERROR
    "authority is not reproducible: two identical runs produced different causal roots\n"
    "  run 1: ${first}\n  run 2: ${second}")
endif()

message(STATUS "authority reproducible: ${first}")
