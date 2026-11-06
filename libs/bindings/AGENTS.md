# bindings — карта правки

Два разных target: `devils_engine::bindings_base` (env.cpp, env.h, Lua/sol2 и базовые
утилиты без Nuklear) и `devils_engine::bindings` (nuklear_bindings.cpp + UI integration).
Человеческого README пока нет: читать CMake и эти файлы.

Общие правила — [корневой AGENTS.md](../../AGENTS.md). Подробное устройство —
заголовки и CMakeLists.txt.

## Границы и контракты

- Headless consumers, в частности originator_lua, подключают bindings_base. Не протаскивать
  visage/Nuklear в базовую среду и не замыкать разделённые зависимости обратно.
- На Linux CMake выбирает lua5.4 через pkg-config; headers и runtime должны соответствовать.
  Не ослаблять поиск до произвольной Lua версии без проверки sol2.
- Sol2 set_function: использовать именованные free/member functions вместо нескольких lambda
  одинаковой сигнатуры из одной функции (коллизия metatable __gc и повреждение при lua_close).
- Sandbox не получает filesystem/package.searchers через обычный require; engine module API
  задаёт consumer. Optional параметры binding должны быть trailing.
- Nuklear context глобален в существующей реализации; multi-context требует явного redesign
  владения, а не только второй sol::environment.

## Проверка

Базовые функции — `originator_queue_lua_test`, `originator_config_test`,
`act_call_context_test` по затронутому API; Nuklear — smoke реального visage consumer.
Отдельного bindings test executable сейчас нет; при существенном новом контракте добавить focused
проверку, а не считать косвенное покрытие полным.

Собирать из корня: `cmake --build build-release --target <test_target> -j 8`, затем
`ctest --test-dir build-release -R '<test_target>' --output-on-failure`.
Выбирать проверки по изменению; полный прогон всей библиотеки ради правки текста не нужен.

## Память и задачи

Открытые работы и сверка прежних списков — [TASKS.md](../../TASKS.md), аудит —
[PROJECT_AUDIT.md](../../PROJECT_AUDIT.md). Исторические подробности доступны через
[указатель архива](../../docs/history/README.md). Новые контракты записывать здесь,
хронологию экспериментов — в отдельном audit/history.
