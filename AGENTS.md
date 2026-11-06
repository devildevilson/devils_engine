# Работа в devils_engine

Экспериментальный C++23 engine/framework. В репозитории одновременно живут рабочие механизмы,
исследовательские площадки и незавершённые проекты. Перед изменением читать ближайший код и
локальный `AGENTS.md`; сохранять направление эксперимента, не вводить новую архитектуру по догадке.

## Карта документации

- [README.md](README.md) — русскоязычная карта библиотек для человека.
- `libs/<name>/AGENTS.md` — карта правки конкретной библиотеки: файлы, контракты, проверки.
- [TASKS.md](TASKS.md) — текущая очередь и результаты сверки старых задач.
- [PROJECT_AUDIT.md](PROJECT_AUDIT.md) — аудит 2026-11-07, свидетельства и границы проверки.
- [ROADMAP.md](ROADMAP.md) — каталог движковых контрактов; [ROADMAP_ULT.md](ROADMAP_ULT.md) — дальний горизонт.
- [subprojects/playgrounds/README.md](subprojects/playgrounds/README.md) — каталог площадок;
  [PLAYGROUNDS.md](PLAYGROUNDS.md) — история кампаний и предложения.
- `NETWORKING.md`, `NETWORKING_COMPONENTS.md`, `NETWORKING_STATUS.md` — networking design и журнал.
- `ORIGINATOR_ULT.md`, `ORIGINATOR_GPGPU.md` — направления генератора; фактический API — в
  `libs/originator/README.md`, заголовках и тестах.
- [Архив прежней памяти](docs/history/README.md) — старый AGENTS целиком и указатель по темам.
  Читать нужный фрагмент при необходимости; историческая запись не является текущей инструкцией.

## Границы библиотек

- `options` — compile/link contract, **не** аргументы main; `utils`/`thread` — низкоуровневые механизмы.
- `aesthetics` — ECS; `act` — функции и intents; `mood` — FSM; `acumen` — GOAP;
  `resolve` — порядок массовых взаимодействий; `prefab` — construction рецепты.
- `demiurg` — modules/resources и staged lifecycle; `simul` — host, broker, время и publication.
- `flow` — animation sampling; `input` — устройства/события; `sound` — miniaudio;
  `painter` — rendering; `visage`/`bindings` — Lua/Nuklear UI и отдельно базовая Lua-среда.
- `originator` — typed generation, tools, chains, fields; `network` — caller-driven bounded protocols.
- Предметные grammar, gameplay policy, схемы миров, художественные параметры остаются у consumer.
  Переносить общий механизм после проверки его границы на реальных потребителях.
- Не восстанавливать архивные actor_ref, старый catalogue RPC/replay, painter renderer и OpenAL из
  ignored `exclude/`. Сериализация байтов принадлежит utils; replay/history — network/consumer.

## Текущий контекст генератора

GN01–GN06 демонстрируют разные контракты; pipeline и специализированные инструменты остаются
поддерживаемыми. Наличие generator_chain не требует перевода каждого примера на chain.
GN07 — самостоятельный brief/graph/plan experiment, без линковки originator.
GN09 и GN09 NEXT используют chain, Lua/TAVL/DS; исходный GN09 и NEXT не смешивать.

Для NEXT текущие правила: `subprojects/playgrounds/GN09_local_adventure/NEXT_CAVES_PLAN.md`,
команды и реализация — `NEXT_GENERATOR_GUIDE.md`. Геометрия ждёт визуальной оценки автора.
Не возвращать обязательное узкое горло, гарантии непроходимости лодки, бюджет экспедиции,
ресурсы/монстров или per-room fitting из старой памяти. Проверка структуры не доказывает
естественность; прежние цифры производительности не являются сегодняшней приёмкой.

## Как менять код

- [CODE_STYLE.md](CODE_STYLE.md) — стиль: UTF-8/LF, 2 пробела, snake_case для своего кода;
  именование стороннего API сохраняется. Русские комментарии и design notes допустимы.
- Сначала проверить `git status --short`. Сохранять существующие изменения; не коммитить и не
  откатывать чужую работу. История со словом «uncommitted» не заменяет проверку текущего Git.
- Публичные headers — `libs/<name>/include/devils_engine/...`; зависимости объявлять у target,
  не полагаться на случайные transitive includes. Базовые слои не должны тянуть UI/Vulkan.
- Не менять seed semantics, canonical bytes, lifetime, ordering и concurrency ради уборки.
  При намеренном изменении назвать новую гарантию и подходящий consumer-тест.
- sol2: биндинги через именованные функции/методы; одинаковые сигнатуры разных lambda из одной
  функции могут столкнуться в metatable `__gc`. См. `libs/originator/AGENTS.md`.
- Новая запись в AGENTS — актуальный контракт/ловушка/команда. Длинные протоколы измерений и
  хронологию помещать в audit/history, задачи — в TASKS; не наращивать корневую память журналом.

## Сборка и проверка

Команды выполнять из корня. Существующие build-debug/build-release имеют FetchContent dependencies;
сначала использовать их, не перенастраивать весь toolchain без необходимости.

```sh
cmake --build build-release --target <target> -j 8
ctest --test-dir build-release -R '<focused expression>' --output-on-failure
```

Тесты находятся в `subprojects/tests`, не в корневом `tests/`. Имена/число узнавать через
`ctest --test-dir build-release -N`; сводная build-цель — `devils_engine_test`.
Проверять затронутый механизм и consumer; повтор Debug/Release нужен при риске для bytes,
concurrency или floating-point contracts. GPU-тест без доступного устройства может пропустить
сверку. Визуальные viewers без `--frames` интерактивны и сами не завершаются.
Не переснимать frozen baselines ради документальной уборки. Результаты старых проверок
подписывать датой, новые проверки — фактическими командами и ограничениями.
