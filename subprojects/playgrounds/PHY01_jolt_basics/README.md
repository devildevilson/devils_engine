# PHY01 — Jolt basics and integration seam

Первая headless-площадка физики. Она не объявляет готовый `libs/physics` API: сначала проверяет
закреплённый Jolt 5.6.0 и показывает минимальную границу владения, которую затем можно вынести в
движок без копирования backend-типов во все игровые системы.

## Запуск

```bash
cmake --build build-debug -j4 --target PHY01_jolt_basics
./build-debug/subprojects/playgrounds/PHY01_jolt_basics/bin/PHY01_jolt_basics --verify
```

Полезные ручки: `--steps=N`, `--bodies=N`, `--threads=N`. Число в `--threads` — именно worker
threads Jolt; вызывающий поток также исполняет jobs.

## Что проверяет первый срез

- один process-wide owner для allocator hooks, `Factory` и регистрации типов;
- один world owner для layer tables, `PhysicsSystem`, временного allocator и job system;
- внешний маленький `body_handle`, не позволяющий случайно смешать `BodyID` разных миров;
- static floor, динамические box/sphere bodies, impulses, contacts и fixed update 60 Hz;
- narrow-phase raycast, возвращающий внешний handle;
- побитовое равенство снимка после одинакового прогона с 0 workers и с несколькими workers;
- общие deterministic `Sin/Cos` вместо platform libm и стабильная битовая сигнатура corpus.

## Второй срез

- immutable `shape_catalog` готовит box/sphere shapes отдельно от миров и владеет ref-counted
  cooked result; catalog-specific handle отказывается разрешаться в чужом каталоге;
- body command сразу получает world-specific handle, но не виден queries до `publish_pending()`;
- commit создаёт тела в canonical command order и публикует их одним
  `AddBodiesPrepare`/`AddBodiesFinalize`; поскольку Jolt вправе переставить batch scratch, mapping
  handle → `BodyID` хранится отдельно и не выводится из порядка этого scratch;
- queries-only world не создаёт 8 MiB temp allocator, worker threads или job system и не делает
  `PhysicsSystem::Update`; до commit ray/point-overlap ничего не видят, после commit видят одно тело;
- общие `utils::deterministic::sin/cos` адаптированы из Jolt и побитово сверяются с оригиналом;
  fixture теперь подаёт impulses через общий math слой; их Cody-Waite reduction имеет явный
  диапазон `abs(angle) <= 100000`, а не наследует неопределённый huge-angle corner case;
- общий `utils::deterministic_sort` адаптирован из Jolt, фиксирует перестановку равных элементов и
  уже заменил `std::sort` на canonical map path сериализации.

## Третий срез

- внешний `body_handle` теперь содержит world token, slot и generation. Удаление немедленно
  инвалидирует старый handle; переиспользованный slot получает следующую ненулевую generation;
- capacity exhaustion во время `CreateBody` откатывает все созданные Jolt bodies, не публикует ни
  один slot и оставляет command batch доступным для исправления и повторного commit;
- опубликованное тело можно удалить, а pending-команду отменить через один lifecycle seam;
- `BodyCreationSettings::mUserData` несёт slot+generation. Поэтому mapping после
  `AddBodiesPrepare`, который вправе переставить scratch, восстанавливается по identity, а не по
  позиции в массиве;
- contact callbacks только записывают compact events под mutex в заранее подготовленный bounded
  buffer. После `Update` caller переводит их во внешние handles и сортирует по полному стабильному
  ключу; arrival order worker threads наружу не выходит;
- отдельный cache body/sub-shape pair сохраняет identity для `OnContactRemoved`, где Jolt прямо
  запрещает обращаться к уже уничтоженным bodies. Overflow и unresolved removal являются явной
  ошибкой проверки, а не тихой потерей события;
- serial и four-worker прогоны сравнивают теперь не только state/ray, но и весь contact stream с
  сохранёнными границами tick.

Layer-derived broadphase filter строится только после заполнения исходных layer tables: его
конструктор снимает с них snapshot. Проверка требует хотя бы один contact, поэтому неверный порядок
инициализации не превращает fixture в незаметный тест свободного падения.

`CROSS_PLATFORM_DETERMINISTIC` включён для Jolt. Общий `devils_engine::options` компилирует C++ код с
`-ffp-contract=off` (GCC/Clang) или `/fp:precise` (MSVC), как требует контракт Jolt. Это ещё не
доказательство между платформами: текущий test сравнивает разные расписания jobs в одном binary.
Настоящая проверка потребует сохранить ожидаемый snapshot/hash и прогнать его на GCC/Clang, x86/ARM
и Windows/Linux.

## Что узнали про math

Jolt даёт не отдельный fixed-point scalar для gameplay, а согласованный floating-point math path:
SIMD vectors/matrices/quaternions и собственные `Sin`, `Cos`, `Tan`, `ATan2`, sort/heap/hash helpers.
Их имеет смысл применять внутри physics-facing вычислений вместе с deterministic build flags. Делать
`JPH::Vec3` общей математикой ECS/renderer пока рано: тип выровнен и оптимизирован под solver, а
cross-platform determinism всё равно требует одинакового порядка входных команд, FPU state и
канонического порядка результатов callbacks/queries.

## Следующая граница

Перед переносом в `libs/physics` площадке ещё нужны shape/mesh cooking budgets и явная transform
authority между ECS и physics: кто и в какой boundary пишет position/rotation, как teleport и
kinematic target входят в command stream, и когда результат solver публикуется обратно. Нужны также
batch removal и политика 32-bit generation wrap; текущий handle гарантирует отказ старого поколения,
но после полного оборота счётчика прежнее значение математически повторится.

Пока используется примерный `JPH::JobSystemThreadPool`. Подключить существующий `thread::atomic_pool`
тонким callback недостаточно: Jolt создаёт динамический dependency graph, держит ref-counted jobs и
добавляет jobs в barrier уже во время ожидания, тогда как engine pool сейчас даёт bounded submit/wait
без этой модели. Если объединять pools, нужен отдельный `JobSystemWithBarrier` adapter с измеренной
очередью и доказанным отсутствием deadlock; это самостоятельный следующий срез.

Abseil остаётся vendored dependency protobuf. `absl::InlinedVector` реален, но один контейнер не
оправдывает второй публичный vocabulary type рядом с `std::vector` и GTL; flat hash/btree containers
уже покрыты GTL. Конкретный кандидат на замену должен сначала выиграть benchmark и уменьшить, а не
расширить dependency surface.
