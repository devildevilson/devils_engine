# PF05 — scene effects

Статус: **CLOSED** (2026-08-23). Все запланированные независимые срезы закрыты; художественная сборка выбранных
техник продолжается в `PF06_submarine_light_room`.

Независимая effect gallery между базовыми renderer capabilities и художественными project-сценами.
Она проверяет небольшие пространственные эффекты, которым тесно внутри post-processing или stencil lab,
но которые ещё не должны проектироваться сразу под конкретную игру.

## Запуск

```bash
cmake --build build-debug --target PF05_scene_effects -j2
./build-debug/subprojects/playgrounds/PF05_scene_effects/bin/PF05_scene_effects
```

Опции: `--validation`, `--uncapped`, `--fixed-camera`, `--fixed-step`, `--no-decals`, `--no-particles`,
`--no-weather`, `--weather=clear|rain|snow`, `--no-particle-collision`, `--no-weather-shelter`,
`--no-cel`, `--cel-bands=2..8`, `--cel-softness=0..0.49`,
`--cel-outline=off|silhouette|feature`, `--no-cel-outline`, `--emitter-stop-frame=N`, `--frames=N`,
`--no-world-ui`, `--world-ui-occlusion-fixture`, `--world-ui-selected=<id>`, `--dump=file.ppm`. Камера — WASD/QE,
мышь и Shift;
`F` переключает decal pass, `P` останавливает/запускает
emitter, `R` очищает spark pool, `T` циклически меняет rain/snow/clear, `C` включает collision, `H` — shelter.
`G` независимо переключает cel lighting, `B` циклически меняет 2–5 bands, `O` — outline policy, `U` — world UI.
`I` переключает camera capture на обычный GLFW `CURSOR_NORMAL`; в этом режиме клик по окну выбирает связанный
`window_id`, клик мимо снимает выбор. Возврат из режима не создаёт ложный mouse delta.
При `--fixed-camera`
cursor возвращается в `CURSOR_NORMAL`, а `--dump` записывает точный кадр из `scene_color`, как в PF03/PF04.
`--fixed-step` фиксирует только particle timestep на `1/60 s`, чтобы lifecycle rail не зависела от скорости машины.

## Первый срез — Crimson MSDF в мире

Один Crimson MTSDF atlas теперь имеет два равноправных consumer'а. Обычный Visage/Nuklear overlay продолжает
строить собственный UI vertex stream, а PF05 читает из того же `font_t` glyph plane bounds, advance, atlas UV
и line-height. `playground::visage_overlay::font_metrics()` даёт read-only доступ к метрикам; второй atlas и
вторая трактовка шрифта не создаются.

World glyph — инстанс unit quad со следующими данными:

```text
glyph transform + atlas UV rect + fill color + outline color
                + boldness / outline width / softness / atlas slot
```

Fragment shader повторяет смысл MSDF-ветки `ui.frag.glsl`: median RGB восстанавливает signed distance,
`fwidth` переводит четырёхпиксельный range атласа в текущий экранный масштаб, alpha MTSDF используется для
внешней обводки. Поэтому размер world quad не требует отдельного font atlas, а bold/outline/softness не зависят
от способа размещения текста.

В fixture одновременно видны четыре контракта:

- фиксированный `font_height` + `max_length`: размер glyph/advance остаётся заданным, layout перестаёт принимать
  целые glyph'ы перед первым advance, который вышел бы за отрезок;
- только `max_length`: сначала измеряется строка при единичной EM-height, затем
  `font_height = max_length / natural_width`, и вся строка занимает отрезок;
- quadratic Bézier: небольшая arc-length table переводит pen distance в параметр `t`, каждый glyph получает
  отдельную world matrix из точки кривой, касательной и up внутри плоскости текста;
- screen-facing billboard: greedy word wrap выполняется в локальных координатах billboard, а vertex shader
  применяет один из трёх способов размещения вокруг world anchor.

### Три billboard space

`anchor.w` fixture использует как компактный mode; production object record получит обычный enum:

| Mode | Что остаётся фиксированным | Размер на экране | Depth |
| --- | --- | --- | --- |
| spherical | ничего: right/up берутся из camera frame | world units, уменьшается с расстоянием | anchor/glyph world depth |
| cylindrical Y | world Y; к камере поворачивается только горизонтальная ось | world units | обычная world depth |
| world-anchored screen-size | world anchor, а glyph offsets добавляются в clip XY как pixels | постоянный, fixture = 38 px | depth world anchor |

Последний режим — не обычный UI: `anchor_clip = VP · anchor`, затем
`clip.xy += pixel_offset · 2/viewport · anchor_clip.w`. Множитель `w` принципиален — без него размер снова
зависел бы от расстояния. Сейчас pipeline сохраняет anchor Z/W и делает reverse-Z test. Если health/name marker
должен быть виден сквозь мир, это будет второй material policy без depth test, а не четвёртая billboard math.

World и billboard materials теперь также пишут reverse-Z depth. MSDF fragment делает `discard` при практически
нулевом coverage: иначе невидимая часть каждого прямоугольного glyph quad стала бы depth-occluder. Поэтому
ближний spatial label честно закрывает дальний, а world-anchored screen-size label исчезает за стеной. Для
полупрозрачных пересекающихся labels всё равно понадобится явно выбрать sorted-alpha/OIT policy; один depth
buffer сам по себе не определяет правильный порядок смешивания.

### Текстурированный fill

PF05 загружает маленькую procedural weathered-stone texture и подмешивает её в fill нескольких строк, не меняя
MSDF coverage и outline. Дополнительных instance bytes нет: бывшее float-поле atlas slot теперь является
bit-cast словом `{atlas:8, detail:8, mix:8, flags:8}`. Для строк без detail fragment не делает второй sample.
Fixture UV повторяет texture внутри каждого glyph; непрерывный рисунок через всю строку позже потребует передать
text-local pen coordinate, а не использовать quad-local `0..1`.

Startup checks закрепляют два главных layout-инварианта: fit-mode действительно потребляет заданную длину,
а fixed-mode не выходит за лимит и действительно отсекает хвост тестовой строки. Fixed-camera четырёхкадровый
dump с Vulkan validation проходит чисто.

Layout декодирует UTF-8 в codepoints, но текущий общий atlas packer по-прежнему загружает только ASCII charset
(95 glyph metrics); отсутствующий codepoint закономерно попадает в fallback glyph. Расширение charset — отдельный
font-resource вопрос, а не ограничение world placement.

## Production-направление text data

Текущий proof заранее строит полную `mat4` на CPU и поэтому прост, но тяжёл: 128 bytes/world glyph и
144 bytes/billboard glyph. Переносить на GPU просто строку `uint32 rune[]` недостаточно. Для реального языка
rune не равна glyph: shaping должен учесть ligatures, kerning, combining marks, bidi и fallback font.

Предпочтительный контракт после измерения второго consumer:

```text
CPU: UTF-8 → script/language shaping → glyph_id + advance/offset

font GPU table (один раз): glyph_id → plane bounds + atlas rect + advance
text object record: transform/anchor + curve + style + clipping/wrap policy
per-glyph stream: glyph_id + pen distance + line offset + flags       ≈ 16 bytes

vertex shader: fetch metrics → line/curve/billboard placement → 4/6 vertices
fragment shader: MSDF coverage + optional material texture
```

Prefix sum advances и Unicode shaping остаются CPU-работой для редко меняющихся labels. Для тысяч часто
изменяющихся строк можно позже добавить compute-expansion из уже SHAPED glyph stream; заставлять каждый vertex
заново суммировать advances предыдущих glyph'ов было бы хуже текущего решения.

Общий font core действительно должен выйти из `visage`: чистые face/metrics/charset/fallback/atlas data с
минимальными зависимостями, поверх него отдельный Visage/Nuklear adapter и Painter GPU adapter. Сейчас
`font_t` ещё содержит `nk_user_font`, поэтому прямое перемещение типа только усилило бы связанность.

Для произвольной 3D Bézier одного постоянного `plane_normal` недостаточно: frame начнёт крутиться или выродится.
Общий вариант должен parallel-transport предыдущий up вдоль касательной; для подписи страны на планете лучшим
up-provider является radial/surface normal, после чего right строится из tangent и surface normal. Сама точка и
производная quadratic/cubic Bézier уже являются общей CPU/GLSL математикой в `utils/shared.h`; PF05 добавил туда
обе derivative-функции. В `utils/shared.h` стоит продвигать только одинаковую CPU/GPU математику и packing,
а descriptor/pipeline contracts должны оставаться в Painter shader includes/`libs/painter`, чтобы shared header
не превратился в склад эффектов.

## Screen-space decals

Второй срез использует именно deferred/screen-space projection, а не quad, лежащий рядом со стеной:

```text
opaque scene -> scene color + depth + world normal
oriented decal box back faces
  -> depth sample -> inverse(VP) -> world position -> world_to_decal
  -> reject outside local [-0.5, 0.5]
  -> reject/fade by receiver normal
  -> local XY -> decal/MSDF UV -> alpha blend into scene color
world/billboard text with writable depth -> UI -> present
```

Fixture проецирует Crimson-надписи на дальнюю и правую стены, используя два разных ортонормальных basis.
Один glyph сейчас равен одному volume instance, потому что glyph'ы лежат в разных областях общего atlas;
обычная картинная decal использовала бы тот же pass с одним volume и одним цельным UV rect. Instance хранит
`decal_to_world` для rasterization и заранее вычисленный `world_to_decal` для fragment clipping — инвертировать
матрицу на каждом пикселе незачем. Back-face rasterization ограничивает fragment work экранной проекцией box,
а проверка локальных координат остаётся авторитетной.

Normal threshold `0.55` не даёт проекции загнуться с дальней стены на боковую грань. Decal не тестирует и не
пишет hardware depth: она получает уже выбранную opaque surface из `scene_depth`, так что закрытая стеной
поверхность естественно не получает эффект. После decal pass граф переводит depth из read-only layout обратно
в attachment layout для текста, который уже должен писать глубину.

Граница техники на этом срезе явная: decals получают только opaque depth/normal и меняют только scene color.
Transparent receivers, normal/roughness modification и angle-independent projection — отдельные material
policies; случай камеры внутри volume текущим fixture отдельно не проверен.
`--no-decals` является честным A/B: при одинаковом fixed-camera кадре исчезают обе projected надписи.

## Particles, emitter lifecycle и простая физика

Третий и четвёртый срезы используют один persistent GPU buffer на 3072 stable slots: 2048 для point-emitter
sparks и 1024 для выбранного weather mode. CPU не пересылает массив
частиц обратно и вперёд: каждый кадр он передаёт только параметры emitter, timestep и маленький spawn batch
`{first_serial, count}`. Сам `particle_state` является `per_frame` buffer; compute пишет текущую копию и читает
`history = 1`, поэтому Painter выводит и внутрикадровый compute→vertex barrier, и cross-frame зависимость.
Compute теперь стоит после opaque scene pass, потому что collision читает current-frame depth/normal.

```text
CPU emitter: rate accumulator -> contiguous spawn serial range
GPU spark slot: previous -> integrate -> analytic + screen collision -> expire/spawn -> current
GPU weather slot: previous -> rain/snow motion -> screen collision -> camera-local respawn -> current
graphics: 2048 additive sparks + 1024 alpha weather billboards -> opaque depth test
```

Один invocation полностью владеет одним slot, поэтому здесь нет freelist, compaction и atomics. Spawn serial
по modulo выбирает slot и одновременно служит стабильным random seed. При текущих `180 particles/s` и
`2.4..4.2 s` lifetime максимально ожидаемое число живых частиц заметно меньше capacity, поэтому новый spawn
не должен догонять ещё занятой slot.

Lifecycle разделён сознательно:

- `emitting`: rate accumulator выдаёт новые serials;
- `draining`: spawn прекращён, но уже живые particles продолжают update;
- `stopped`: прошло не меньше максимального lifetime, следовательно pool гарантированно пуст;
- `R`: одноразовый reset flag заставляет compute игнорировать историю всех slots и снова начать emission.

Физика — semi-implicit Euler: сначала gravity и exponential drag меняют velocity, затем обновляется position.
Пол и четыре границы комнаты являются аналитическими planes; пересечение исправляет position и отражает нужную
компоненту velocity с restitution, на полу дополнительно гасится горизонтальная скорость.

Screen-space collision дополняет их произвольной видимой геометрией. После интегрирования candidate position
проецируется в текущий viewport, shader читает ближайшие `scene_depth + scene_normal` и восстанавливает world hit.
Отрезок считается пересёкшим поверхность, если previous point был с camera-side, candidate дошёл до surface-side,
остался внутри небольшой thickness и velocity направлена в receiver normal. Spark отражает velocity, rain/snow
завершаются и переиспользуют slot. `C`/`--no-particle-collision` дают A/B.

Это не world collision structure: за первой видимой поверхностью, вне экрана и для occluded geometry данных нет;
один candidate-depth sample также может пропустить очень быстрый или тонкий контакт. Current-frame порядок убирает
лаг камеры, thickness покрывает обычный `1/60 s` шаг, но gameplay particles всё равно требуют colliders/SDF/BVH.

Weather mode переиспользует одни 1024 slots, но является двумя разными consumer:

- rain рождается в camera-local box, быстро падает с ветром и рисуется узкими голубыми streaks. Его длинная
  billboard-ось совпадает с world velocity, а к камере вокруг неё поворачивается только поперечная ось: это
  цилиндрический ribbon, который перспективно сокращается при взгляде вдоль падения и не становится camera-up полосой;
- snow падает медленно, получает меняющийся боковой drift и рисуется мягкими холодными flakes;
- clear немедленно освобождает weather range; смена mode сбрасывает только эту часть buffer.

Одной screen-space depth недостаточно для дома: невидимая или offscreen крыша не существует для такого collision.
Поэтому emitter дополнительно принимает явный axis-aligned `shelter min/max`. Вся precipitation внутри volume
удаляется до render, а scattered spawn внутри него сразу отвергается. Fixture трактует комнату `[-5..5] ×
[-1.5..3.5] × [-5..3]` как один dry volume и получил потолок при открытом входе: снаружи дождь виден, изнутри
он остаётся только за проёмом. `H`/`--no-weather-shelter` дают прямой A/B. Один AABB — намеренно простой контракт;
несколько зданий потребуют маленький buffer volumes либо spatial index, а сложный навес можно собрать из нескольких
convex volumes.

Отдельная rail отключает screen collision в обоих кадрах: shelter ON/OFF всё равно меняет `415.191`
pixel-equivalent в crop входа/комнаты. Повтор shelter-кадра вне overlay даёт `AE=0`, поэтому volume не является
случайной маскировкой depth test и сохраняет детерминированный particle path.

Даже после тонкой геометрии rain look не исчерпан частицами. Полная сцена обычно сочетает как минимум near drops,
дешёвую mid/far volumetric density или rain fog, wetness/ripples на surfaces и impact splashes. PF05 сейчас доказывает
только первый уровень и управление осадками; атмосферное рассеяние и wet-surface response относятся к lighting/PBR.

Spark renderer использует spherical billboards, мягкий radial coverage и order-independent additive blending.
Weather намеренно использует обычный alpha blend: это не заставляет снег светиться, но честно оставляет проблему
сортировки при большой плотности; production выбирает sorting, weighted OIT либо допустимые артефакты. Все
particles тестируют opaque reverse-Z depth, но сами depth не пишут. UI вынесен в отдельный последний pass,
чтобы плотный snow никогда не мог оказаться поверх diagnostics.

Детерминированная проверка (`--fixed-step --uncapped --weather=clear --emitter-stop-frame=60`) показывает lifecycle численно:
на кадре 180 draining-кадр ещё отличается от `--no-particles`, а на кадре 360 после максимального lifetime
вне overlay совпадает с ним (`AE = 0`). Два независимых emitting dump кадра 180 также совпадают (`AE = 0`).
Vulkan validation проходит чисто.

Weather rails также воспроизводимы: повтор rain frame 180 даёт `AE=0`; collision ON/OFF меняет `2510.13`
pixel-equivalent вне overlay, а rain/snow — `5648.02`, то есть mode и collision не являются пустыми тумблерами.

Текущие fixed draws вызывают шесть vertices для всех 3072 slots, а мёртвые уходят за clip volume. Это хороший
неатомарный baseline; GPU compaction + indirect instance count следует добавлять после измерения цены пустых
slots или появления существенно больших pools. `--no-particles` — debug runtime switch и не удаляет passes;
production `particles = off` должен выбирать graph generation без simulation/render steps.

## Cel shading и outline policy

Пятый срез отделяет две runtime-настройки, которые часто ошибочно склеивают в один `toon material`.
`cel_settings_buffer` обновляется на следующий кадр и содержит lighting mode, число bands/softness, outline policy,
thresholds и цвет. Никакой pipeline или graph rebuild для этих значений не нужен.

Opaque shader сначала считает обычный directional Lambert `N·L`. В cel mode диапазон `0..1` превращается в
`N` равномерных уровней; граница между соседними уровнями получает короткий `smoothstep`, заданный в долях одной
ячейки. При softness `0` переход математически жёсткий, но сильнее shimmering; `0.08` оставляет полосу визуально
дискретной и даёт небольшой AA-переход. Ambient добавляется после квантования, поэтому тёмный lighting band не
превращается в абсолютный чёрный. Отдельная гладкая UV-сфера справа существует именно как fixture: кубы не могут
показать несколько bands внутри одной грани.

Outline является screen-space consumer уже готовых opaque `scene_depth + scene_normal`:

- `off` полностью отбрасывает fullscreen pass;
- `silhouette` ищет границу geometry/background и относительный разрыв linear depth;
- `feature` добавляет normal discontinuity и подчёркивает видимые жёсткие грани.

Относительный depth threshold работает одинаковее вблизи и вдали, круглая окрестность `2 px` сохраняет экранную
толщину. Pass стоит после opaque lighting, но перед decals, particles и text: контур принадлежит видимой поверхности
мира, а последующие слои могут его закрыть. Он сознательно не является PF04 through-wall selection outline, не знает
скрытой геометрии и не обводит прозрачные particles/MSDF. При policy `off` runtime pass пока остаётся в graph и
делает discard; production `cel_outline = off` должен собирать generation без этого pass.

Fixed-camera rail без decals/particles различает настройки вне overlay: smooth Lambert против 2 bands даёт
`AE=17417`, 2 против 5 bands — `20338`, silhouette против feature — `493.542`. Повтор feature-кадра совпадает
побитно в проверяемом crop (`AE=0`); Vulkan validation чистая.

## Native Nuklear world UI

Шестой срез строит маленькие окна над объектами через отдельный native Nuklear context — Lua в этом пути не
участвует. Вход намеренно уже общего immediate-mode API: `world_ui_window` содержит стабильный id, world anchor,
имя, health, до трёх коротких `{label,value,color}` строк и до трёх texture slots; общий `world_ui_style` задаёт
размер окна, font size `18 px`, отступ от anchor и лимиты. Текущий fixture передаёт массив трёх окон за кадр:
две строки состояния, health bar с green/amber/red policy и одну-две картинки из обычного bindless UI descriptor.

Все N окон вызывают Nuklear, а затем проходят ОДИН `nk_convert`. Стандартная 20-byte UI vertex расширяется одним
`uint window_id`; id берётся из command userdata и запекается в индексируемые этой командой вершины. Raw pointer на
матрицу намеренно не переживает convert/upload: GPU получает маленький id, а отдельный SSBO хранит по 48 bytes на
окно — `{world anchor, fade end}`, `{pixel offset, size}` и distance policy. Поэтому texture/font по-прежнему создают обычные draw
commands, но матрица не размножает render calls. Fixture сейчас даёт 722 vertices и 15 texture/solid commands для
трёх окон.

У Nuklear обнаружилась важная тонкость: автоматический background создаётся внутри `nk_begin`, до того как функция
возвращает current window, и соседние solid primitives могут слиться под userdata предыдущего окна. Поэтому implicit
background прозрачен, а видимая panel/background border рисуется сразу после `nk_begin` уже с правильным userdata.
Иначе следующая панель появлялась второй раз со сдвигом ровно на шаг virtual layout.

Vertex shader проецирует anchor обычной camera VP и прибавляет local pixel offset к `clip.xy`, умножая его на
`clip.w`. Но размер теперь не строго постоянный: `scale = clamp(reference_distance / view_depth, min, max)` сочетает
поведение spherical/world-size и screen-size billboard. При текущей policy reference = `5.5 m`, clamp =
`0.45..1.35`; близкое окно растёт вместе с объектом, дальнее уменьшается, но оба остаются читаемыми. После `13 m`
alpha плавно уходит в ноль к `16 m`. CPU использует ту же проекцию и scale для hit rectangle, а при overlap выбирает
окно с минимальным view depth. Stable строковый `window.id` остаётся gameplay identity, тогда как плотный индекс
нужен только GPU batch. В fixture ids обратно связаны с двумя box instances и sphere: selection слегка сдвигает
материал объекта к cyan, а выбранное окно получает более светлые panel и border.

Все окна сохраняют reverse-Z depth anchor.
Материал тестирует и пишет depth с `greater_or_equal`: opaque geometry закрывает окно, а ближнее окно закрывает
дальнее. Fragment shader делает `discard` при почти нулевой MSDF/image alpha, чтобы прозрачная площадь glyph quad
не стала невидимым depth occluder. World UI исполняется до финального diagnostic overlay.

`U`/`--no-world-ui` дают A/B `3954.75` pixel-equivalent вне overlay, повторный кадр совпадает (`AE=0`). Новый
distance кадр явно различает размеры трёх окон; `--world-ui-selected=pf05.sentinel` меняет `2481.53`
pixel-equivalent в общем crop панели и объекта. Отдельная
`--world-ui-occlusion-fixture` оставляет окно за задней стеной; его кадр побитно совпадает с отключённым world UI
в scene crop (`AE=0`), то есть window действительно проходит world depth, а не только визуально лежит рядом.
Полный кадр с particles/weather проходит Vulkan validation чисто.

CPU picking намеренно геометрический: он знает rectangle и порядок anchor depth, но не читает scene depth. Поэтому
строго запретить клик по полностью закрытой стеной панели сможет только отдельный GPU object-id attachment/readback
либо общий scene ray query; подменять это PF05-specific collision не стали.

Оставленные production-задачи: screen-edge clamp/leader line, overlap avoidance между множеством окон,
occlusion-aware picking, DPI/accessibility policy и внешний parser для этого bounded config. При сотнях labels CPU
`nk_convert` и 16-bit index stream нужно измерить против специализированного glyph/quad generator; этот срез
доказывает удобный путь для десятков маленьких информативных окон, а не универсальный UI scene graph.

### Что переносить в основной движок

Сейчас уже доказаны существующие общие контракты Painter: один `draw_ui` stream умеет обслуживать N transformed
окон, bindless solid/MSDF/image path переиспользуется без нового вида draw command, а mouse buttons проходят через обычный `input::events`. Их расширять специально под PF05 не требуется.

Следующие кандидаты на перенос требуют второго consumer, чтобы не зацементировать форму лаборатории:

- чистая функция `project_world_ui(anchor, view, projection, viewport, distance_policy)`, возвращающая clip anchor, scale/fade и hit rectangle; CPU и shader должны иметь один проверяемый математический контракт;
- общий native-Nuklear converter, который сохраняет command userdata в дополнительном vertex payload и делает один convert для нескольких поверхностей;
- независимая font face/metrics/atlas library вне `visage`, чтобы world text и native UI не зависели от Lua/Nuklear оболочки diagnostic overlay;
- renderer-wide picking attachment/readback или scene ray query для честного occlusion-aware выбора.

В движок пока не следует переносить `world_ui_window` с полями `health/State/Armor`, цвета, лимит в три строки,
distance числа и реакцию игры на selection: это project-owned presentation policy. Screen-edge/overlap policy тоже должна сначала появиться в реальной сцене с большим числом объектов.

## Планируемые срезы

- ~~Crimson MSDF вдоль отрезка и quadratic Bézier, фиксированный размер/ограниченная длина и wrapped billboard~~;
- ~~spherical, cylindrical-Y и world-anchored screen-size billboards~~;
- ~~screen-space decals с reconstruction из depth/normal и ограниченным decal volume~~;
- ~~particles, emitter lifecycle и простая particle physics~~;
- ~~rain и snow как два наблюдаемо разных consumer particle-системы~~;
- ~~cel shading с управляемыми lighting bands и outline policy~~;
- ~~маленькое world-space UI окно над объектом: имя, health bar и несколько полей состояния~~.

Дополнительные эффекты добавляются только отдельными закрываемыми срезами. Площадка не является одной
обязательной mega-scene и не должна связывать все техники в один pipeline.

## Граница

Каждый срез использует обычные painter resources/materials/render graph и владеет локальными shaders и
fixtures. Production parsing/execution fixes принадлежат `libs/painter`; effect semantics и debug views
остаются в лаборатории до второго реального consumer.

## Definition of Done

У каждого принятого эффекта есть запускаемый наблюдаемый сценарий, фиксированная camera/debug view,
объяснённая граница алгоритма и минимальная проверка, отличающая работающую технику от passthrough.
