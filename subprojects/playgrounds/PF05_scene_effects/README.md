# PF05 — scene effects

Независимая effect gallery между базовыми renderer capabilities и художественными project-сценами.
Она проверяет небольшие пространственные эффекты, которым тесно внутри post-processing или stencil lab,
но которые ещё не должны проектироваться сразу под конкретную игру.

## Запуск

```bash
cmake --build build-debug --target PF05_scene_effects -j2
./build-debug/subprojects/playgrounds/PF05_scene_effects/bin/PF05_scene_effects
```

Опции: `--validation`, `--uncapped`, `--fixed-camera`, `--no-decals`, `--frames=N`, `--dump=file.ppm`.
Камера — WASD/QE, мышь и Shift; `F` переключает decal pass. При `--fixed-camera` cursor возвращается в
`CURSOR_NORMAL`, а `--dump` записывает точный кадр из `scene_color`, как в PF03/PF04.

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

## Планируемые срезы

- ~~Crimson MSDF вдоль отрезка и quadratic Bézier, фиксированный размер/ограниченная длина и wrapped billboard~~;
- ~~spherical, cylindrical-Y и world-anchored screen-size billboards~~;
- ~~screen-space decals с reconstruction из depth/normal и ограниченным decal volume~~;
- particles, emitter lifecycle и простая particle physics;
- rain и snow как два наблюдаемо разных consumer particle-системы;
- cel shading с управляемыми lighting bands и outline policy;
- маленькое world-space UI окно над объектом: имя, health bar и несколько полей состояния.

Дополнительные эффекты добавляются только отдельными закрываемыми срезами. Площадка не является одной
обязательной mega-scene и не должна связывать все техники в один pipeline.

## Граница

Каждый срез использует обычные painter resources/materials/render graph и владеет локальными shaders и
fixtures. Production parsing/execution fixes принадлежат `libs/painter`; effect semantics и debug views
остаются в лаборатории до второго реального consumer.

## Definition of Done

У каждого принятого эффекта есть запускаемый наблюдаемый сценарий, фиксированная camera/debug view,
объяснённая граница алгоритма и минимальная проверка, отличающая работающую технику от passthrough.
