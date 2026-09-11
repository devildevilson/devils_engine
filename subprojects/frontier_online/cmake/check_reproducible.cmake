# Детерминизм причинного состояния авторитета — две проверки, вторая строго сильнее первой.
#
#   1. Два прогона с ОДНИМИ И ТЕМИ ЖЕ аргументами дают один корень.
#      Ловит зависимость от планировщика ОС: полосы scratch раздаются воркерам, и какому потоку
#      достанется чанк, от запуска к запуску разное.
#
#   2. Прогоны на 1/2/4/8 потоках дают ОДИН И ТОТ ЖЕ корень.
#      Ловит всё, что переживает в полосе одну сущность. Именно эта проверка нашла двойное
#      освобождение в acumen::astar::free_all: один провалившийся поиск замыкал список свободных
#      узлов пула в цикл и отравлял полосу до конца её жизни.
#
# Мир берётся С ДОБЫЧЕЙ намеренно. У префаба prey отключены chase/eat, поэтому его поиск может
# НЕ НАЙТИ план — а отравлялся контейнер ровно на пути неудачи. Стенды, спавнящие только `actor`,
# проходили и со сломанным движком: состав мира в MT-тесте это часть контракта, а не деталь.

if (NOT DEFINED FRONTIER_SERVER)
  message(FATAL_ERROR "check_reproducible: FRONTIER_SERVER is not set")
endif()

set(WORLD --ticks=200 --actors=256 --prefabs=prey,prey,prey,actor --verify)

function(root_for workers output_variable)
  execute_process(
    COMMAND ${FRONTIER_SERVER} ${WORLD} --workers=${workers}
    OUTPUT_VARIABLE out
    ERROR_VARIABLE err
    RESULT_VARIABLE code)
  if (NOT code EQUAL 0)
    message(FATAL_ERROR "authority (workers=${workers}) exited with ${code}\n${out}\n${err}")
  endif()
  if (NOT out MATCHES "state\\.root = ([0-9]+)")
    message(FATAL_ERROR "authority (workers=${workers}) printed no state.root\n${out}")
  endif()
  set(${output_variable} "${CMAKE_MATCH_1}" PARENT_SCOPE)
endfunction()

root_for(4 first)
root_for(4 second)
if (NOT first STREQUAL second)
  message(FATAL_ERROR
    "authority is not reproducible: two identical runs produced different causal roots\n"
    "  run 1: ${first}\n  run 2: ${second}")
endif()

set(reference "")
foreach (workers IN ITEMS 1 2 4 8)
  root_for(${workers} root)
  if (reference STREQUAL "")
    set(reference "${root}")
  elseif (NOT root STREQUAL reference)
    message(FATAL_ERROR
      "causal root depends on the worker count: something in a scratch lane outlives its entity\n"
      "  workers=1: ${reference}\n  workers=${workers}: ${root}")
  endif()
endforeach()

message(STATUS "authority deterministic across 1/2/4/8 workers: ${reference}")
