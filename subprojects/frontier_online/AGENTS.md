# frontier_online — карта правки

Сетевой вариант tile_frontier: headless-авторитет и графические клиенты. Устройство и команды —
[README.md](README.md), общие правила — [корневой AGENTS.md](../../AGENTS.md).

## Где что

| Что | Где |
|---|---|
| мир акторов, тик, хвост тика | `include/core/actor_simulation.h`, `src/core/actor_simulation.cpp` |
| кадр акторов (читатель хвоста тика) | `include/core/actor_batch.h` |
| checkpoint и корень состояния | `include/core/actor_checkpoint.h` |
| земля (originator) | `include/core/terrain.h`, `resources/modules/core/generator/` |
| причинное содержимое и `content_root` | `include/core/causal_content.h`, `include/core/content_root.h` |
| мозги и префабы из ресурсов | `src/core/brain_config_loader.cpp` |
| игровой фасад клиента (камера, тайлы, UI, сеть) | `include/core/frontier_online_game.h` |
| клиент в сессии, окно «network» | `include/core/net_client.h` |
| протокол проекта, авторитет, ведомый, живой рантайм | `include/net/` |
| сроки сети (`network.tavl`) | `include/net/network_settings.h` |
| опции запуска клиента | `include/core/launch_options.h`; сервер — `server_main.cpp` |
| мир сцены (камера, число акторов, генератор) | `resources/modules/core/worlds/frontier_online.tavl` |
| интерфейс | `resources/modules/core/ui/entry.lua` |

## Границы и контракты

- `frontier_causal` не линкует презентацию (painter, visage, sound, input, simul). Сервер линкуется
  только с ним; просочившаяся зависимость ломает сборку сервера — это и есть проверка.
- В проекте остаётся только то, что определяет проект: компоненты, правила взаимодействий, действия
  и цели ИИ, смысл канонических записей, мир и его генератор, раскладка UI. Общий механизм —
  в движок (сеть — `network`/`network_gns`, кадр и съём — `simul`/`painter`, ресурсы — `demiurg`),
  а не копией сюда; проверять границу на втором потребителе (tile_frontier, NETLAB01).
- Корень состояния — канонические байты checkpoint. Не менять порядок, seed semantics и байты ради
  уборки; намеренное изменение — с новой `protocol_version`/отпечатком схемы и тестом.
- Земля по сети не едет: менять генератор — значит менять отпечаток мира (`terrain_source::fingerprint`
  считает тексты документов). Новый причинный каталог ресурсов вписывать в `causal_content_directories`,
  иначе он выпадет из `content_root`.
- Отказ удалённой стороны, обрыв, рассинхрон — статусы и состояния сессии, не `utils::error`.
  Критическое (свои ресурсы, свои инварианты) — `utils::error` у причины.
- Ресурсы грузит владелец реестра: состояние загрузки (сцена объявляет каталоги с `prepare = true`)
  или `synchronous_loader` у сервера и тестов. Потребитель только проверяет готовность
  (`resource_system::require<T>`, `require_usable`).
- Клиент показывает подтверждённый мир; прогноза нет (решение автора). Не добавлять локальный мир
  сцены в сессии.
- Мир сессии принадлежит авторитету: клиент не читает зерно/размер из своей сцены в сессии.
- Никаких абсолютных путей в конфигах; `--resources` — только внутри папки проекта.
- sol2: биндинги — именованные методы (`frontier_online_game::lua_bindings`), не лямбды.

## Ловушки

- GNS: `try_send` только копирует в слот, закрытие — без linger. Выход сразу после отправки теряет
  сообщение; дефект выглядит как чужая беда (потерянный отчёт, отказ без причины).
- Добровольный уход — слово `leave`: молча закрытое соединение — обрыв, его слот удерживается.
- Тесты параллельны в CI и делят запасной диапазон портов: обрыв имитировать тем же авторитетом
  (`terminate`), не освобождая порт.
- Рендер-граф общий с tile_frontier и тестами (`subprojects/tests/test_render_config`): фикстура
  теста там — боевая настройка обоих проектов.

## Проверка

Сборка из корня: `cmake --build build-release --target frontier_online frontier_online_server -j 8`.
Тесты: `ctest --test-dir build-release -R '^frontier_' --output-on-failure` (список и смысл — README
«Проверки»). Правка причинного кода — ещё `frontier_server_reproducible` и сверка `--trace` до/после.
Правка сети — `frontier_join_smoke`, `frontier_live_tick_smoke`, `frontier_rejoin_smoke`,
`frontier_hardening_smoke`.

Путь boot → loading → game и живой клиент тестами не покрыты. Проверять:

```sh
cd build-release/subprojects/frontier_online/bin
./frontier_online --frames=120 --dump=/tmp/frame.png
./frontier_online_server --live --play --listen=41377 --expect-joins=1 --ticks=3000 &
./frontier_online --join=127.0.0.1:41377 --frames=300 --dump=/tmp/session.png
```

Validation: `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation`, в логе не должно быть `VUID`.
