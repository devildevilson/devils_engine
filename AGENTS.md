# Работа в devils_engine

Экспериментальный C++23 engine/framework. В репозитории одновременно живут рабочие механизмы,
исследовательские площадки и незавершённые проекты. Перед изменением читать ближайший код и
локальный `AGENTS.md`; сохранять направление эксперимента, не вводить новую архитектуру по догадке.

## Карта документации

- [README.md](README.md) — русскоязычная карта библиотек для человека.
- `libs/<name>/AGENTS.md` — карта правки конкретной библиотеки: файлы, контракты, проверки.
- [TASKS.md](TASKS.md) — текущая очередь и результаты сверки старых задач.
- [PROJECT_AUDIT.md](PROJECT_AUDIT.md) — обзор структуры/документации.
- [TECHNICAL_AUDIT.md](TECHNICAL_AUDIT.md) — ресурсы, ошибки, зависимости и измерения 2026-11-07.
- [ROADMAP.md](ROADMAP.md) — каталог движковых контрактов; [ROADMAP_ULT.md](ROADMAP_ULT.md) — дальний горизонт.
- [subprojects/playgrounds/README.md](subprojects/playgrounds/README.md) — каталог площадок;
  [PLAYGROUNDS.md](PLAYGROUNDS.md) — указатель на единый каталог.
- `NETWORKING.md`, `NETWORKING_COMPONENTS.md`, `NETWORKING_STATUS.md` — networking design и журнал.
- `ORIGINATOR_ULT.md`, `ORIGINATOR_GPGPU.md` — направления генератора; фактический API — в
  `libs/originator/README.md`, заголовках и тестах.
- [Engine/project/debug settings](libs/simul/CONFIGURATION.md) — владельцы файлов, overrides и audio modes.
- [Владение и ресурсы](docs/review/LIFETIME_AND_RESOURCES.md) — контексты исполнения, VMA accounting и audio ownership.
- Историю разработки не накапливать в AGENTS; хранить актуальные контракты и воспроизводимые проверки.

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

## Ресурсы и критические ошибки

- Минимальная целевая машина: 8 аппаратных CPU-потоков и устройство с Vulkan 1.0. Это
  независимые требования; восемь — минимум CPU, не предел числа потоков процесса.
- 256 МиБ RAM / 256 МиБ VRAM — диагностические ориентиры. Превышение требует разбора расходов,
  а допустимые бюджеты задаёт конкретный проект; GN09 рассчитан на более тяжёлый сценарий.
- Приёмку ресурсов проводить на целевой конфигурации (от 8 CPU-потоков). Будущие demo/replay
  для таких проверок определяются в конкретных проектах, общего сценария пока нет.
- Worker count не равен общему thread count. Не создавать скрытые независимые pools;
  учитывать scratch/outputs одновременно исполняющихся jobs, Lua states, cache и retiring data.
- Критические ошибки оформлять отдельными понятными `utils::error` у причины: owner/resource/
  step/key, ошибочное значение и граница. Продолжение после них не считается штатной веткой.
- Exceptions не применять как обычный fallback/control flow; painter пока сохраняет исключение
  из этой политики. Не выключать сам механизм C++ exceptions глобальным flag: utils::error и
  сторонние границы ещё его используют.
- Нормальное отсутствие решения, protocol refusal/backpressure и optional capability возвращать
  статусом/данными. Не превращать их в fatal error только ради общей формы API.
- Cleanup/перенос уже критической ошибки через future допустим; swallowing в Lua/host с последующим
  успешным publish — дефект. Диагностический logger сам по себе ещё не гарантирует остановку.
- Открытые P0/P1 и протоколы — в TECHNICAL_AUDIT.md и TASKS.md. Negative audit probes не являются
  зелёной приёмкой engine; не выдавать source-site counts за полное покрытие ошибок.

## Текущий контекст генератора

GN01–GN06 демонстрируют разные контракты; pipeline и специализированные инструменты остаются
поддерживаемыми. Наличие generator_chain не требует перевода каждого примера на chain.
GN07 — самостоятельный brief/graph/plan experiment, без линковки originator.
GN09 (закрыта 2026-11-13) использует chain, Lua/TAVL/DS; прежний генератор удалён, остался мир
`gn09_next`. Решения автора и команды — `subprojects/playgrounds/GN09_local_adventure/README.md`,
устройство — `GENERATOR.md` там же. Естественность геометрии оценивает автор.
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
  результаты проверок помещать в audit/review, задачи — в TASKS; не наращивать корневую память журналом.

## Сборка и проверка

Команды выполнять из корня. Существующие build-debug/build-release имеют FetchContent dependencies;
сначала использовать их, не перенастраивать весь toolchain без необходимости.

```sh
cmake --build build-release --target <target> -j 8
ctest --test-dir build-release -R '<focused expression>' --output-on-failure
```

Тесты находятся в `subprojects/tests`, не в корневом `tests/`. Имена/число узнавать через
`ctest --test-dir build-release -N`. `devils_engine_test_binaries` собирает prerequisites без
запуска; `devils_engine_test` также запускает все engine CTest, включая длинные campaigns.
Новые direct tests объявлять через `devils_add_test` с target или `$<TARGET_FILE:...>`;
optional test и его target защищать одинаковым условием. Doctest использует `devils_add_doctest`.
Проверять затронутый механизм и consumer; повтор Debug/Release нужен при риске для bytes,
concurrency или floating-point contracts. GPU-тест без доступного устройства может пропустить
сверку. Визуальные viewers без `--frames` интерактивны и сами не завершаются.
Не переснимать frozen baselines ради документальной уборки. Результаты старых проверок
подписывать датой, новые проверки — фактическими командами и ограничениями.
