# PF02 — shadows

Независимая лаборатория shadow rendering поверх минимального painter baseline, доказанного `PF01`.
Она не зависит от target или исходников `PF01`: нужный baseline копируется выборочно либо берётся из
уже общего API `libs/painter`/`../common`.

## Цель и границы техник

Цель лаборатории — **лёгкие, но правдоподобные тени без temporal-накопления и без физически корректной
мягкости**. Стохастика, history/reprojection, HZB, полноценный PCSS и area lights намеренно остаются
`PF03`/будущему PBR-срезу: в `PF02` любая техника обязана давать стабильный кадр сама по себе, без
предыдущих кадров.

Ключевой водораздел, вокруг которого построен весь срез:

- **сглаженные тени (edge AA)** — фиксированный фильтр; ширина перехода определяется разрешением карты,
  а не размером источника. Дёшево, детерминированно, не требует temporal. **Это цель `PF02`.**
- **мягкие тени (penumbra)** — ширина зависит от размера источника и расстояния blocker→receiver;
  требует blocker search и/или стохастики с временным накоплением. **Это не цель `PF02`.**

Правдоподобие в этой лаборатории набирается не мягкостью краёв, а тремя дешёвыми составляющими:

1. корректный bias — тень «приклеена» к объекту, нет ни acne, ни peter-panning;
2. интерьер тени не чёрный — ambient/skylight внутри тени;
3. затемнение в контакте — усиление у основания объекта.

Имитация роста мягкости с расстоянием допускается только в дешёвой форме (оценка полутени по одному
центральному tap'у), а не через blocker search.

## Выбранные техники

- **аппаратное сравнение глубины**: shadow atlas сэмплируется `sampler2DShadow` с
  `compareEnable`/`compareOp` и линейной фильтрацией, поэтому один tap возвращает билинейную долю по
  `2×2`; взвешенный `3×3` даёт эффективный футпринт `6×6` дешевле нынешних ручных сравнений;
- **взвешенный детерминированный kernel** (tent/gauss) как режим по умолчанию. Rotated Poisson остаётся
  режимом для A/B: без temporal-накопления случайное вращение читается как статичный дизер, а не как
  мягкость;
- **дешёвая оценка полутени**: один центральный blocker-tap задаёт масштаб радиуса фильтра
  (`penumbra ∝ (d_receiver − d_blocker) / d_blocker`), без 16-сэмпльного blocker search;
- **screen-space contact только как добавка детали** на первые сантиметры контакта, где не хватает
  текселя карты. Карта теней остаётся источником основной тени; вклад комбинируется через `min`, а не
  умножением, чтобы полутень не затемнялась дважды;
- **толщина блокера из данных, а не из константы**: back-depth проход (`cull = front`) даёт реальный
  интервал `[front, back]`, локальная производная глубины — fallback для односторонней геометрии;
- **depth-aware upsample** half-res contact masks вместо обычного bilinear, чтобы маска не протекала
  через силуэты;
- **ambient внутри тени** как обязательная часть картинки, а не как «заглушка до PBR».

Уровни настройки параметров (контракт лаборатории; переход между уровнями — осознанный шаг):

| Уровень | Механизм | Что там живёт |
| --- | --- | --- |
| 1 | `definitions` материала | структурное: набор объявлений, размеры глобальных массивов, выбор алгоритма |
| 2 | specialization constants шага | числовые тиры качества: число tap'ов, размер воркгруппы компьюта |
| 3 | UBO/SSBO | всё, что крутится в рантайме и висит на кнопках: bias, радиусы, режимы, данные источников |
| 4 | push constants | per-draw/per-region: индексы записей, dynamic depth bias |

Правило: пока параметр висит на отладочной кнопке, он остаётся уровнем 3. В уровни 1–2 он переезжает
только став пресетом качества, потому что их изменение стоит пересборки pipeline или SPIR-V варианта.

## Что уже можно потрогать

- самостоятельный `PF02_shadows` executable и TAVL graph;
- `2048 × 2048` reverse-Z directional atlas с четырьмя `1024 × 1024` camera-frustum cascades;
- фиксированный `2048 × 2048` spot atlas: четыре `1024 × 1024` региона в сетке `2 × 2`, записанные
  одним config-defined `draw_regions` step;
- четыре цветных spot lights; один light движется, каждый имеет собственную view-projection matrix;
- открытая сцена с floor/walls, пятью cube casters, наклонным receiver и тонким contact caster;
- practical split (`lambda = 0.68`), rotation-independent cascade sphere, light-space texel snapping и
  12% blend band; `9` включает окраску выбранных каскадов;
- edge anti-aliasing и физическая мягкость разделены: runtime hard/взвешенный PCF/rotated-Poisson
  выбирают фиксированный AA kernel, а независимый spot-PCSS использует emitter radius в мировых единицах;
- shadow atlas читается сравнивающим сэмплером (`shadow_compare`, `greater_or_equal` под reverse-Z):
  один tap `sampler2DShadow` возвращает билинейную долю прошедших сравнение текселей, поэтому взвешенный
  `3×3` даёт эффективный футпринт `6×6`. Тот же атлас параллельно доступен обычным nearest-сэмплером —
  hard-режим A/B и blocker search PCSS требуют сырого значения глубины;
- PCF-kernel взвешен разделимым tent'ом от `pcf_radius`, а не равномерной суммой: переход читается
  сглаженным, а не ступенчатым;
- main-поток консервативно пересекает caster bounds с каждым spot cone, пакует подходящие instance lanes и
  формирует два generic region spans (stage/casters) на источник;
- оба atlas pass используют `draw_regions`: региональная команда отдельно несёт viewport/scissor,
  dynamic depth bias, индекс GPU-записи и диапазон draw-group spans; cascade/spot matrices и light colors
  остаются в обычных storage buffers;
- raster constant/slope остаются raw Vulkan diagnostics; receiver normal offset вычисляется из world-size
  shadow texel конкретного cascade/spot depth, а каждый tap получает receiver-plane depth correction через
  экранные производные; zero/default presets управляют всеми bias-компонентами;
- camera depth prepass питает отдельный half-resolution compute pass: восемь коротких screen-space шагов
  с occupied-ray refinement формируют один directional и четыре spot contact-shadow канала; восстановленная
  receiver plane отсекает grazing-angle self-intersection и blockers с обратной стороны поверхности,
  а `N·L`, cone и range rejection пропускают заведомо лишние rays; короткий `0.24 m` preset закрывает
  bias-gap, но single-depth silhouette остаётся фундаментальным ограничением screen-space метода, поэтому
  этот исследовательский режим выключен по умолчанию и включается через `F`/`--contact`;
- вклад contact-масок ограничивает карту теней через `min`, а не умножается на неё (иначе полутень
  затемняется дважды), и апсемплится по ближайшей глубине: `contact_directional` — формат `sf2`, где
  `.r` = маска, `.g` = линейная view-глубина источника (`.g == 0` = пиксель без геометрии), поэтому
  обычный bilinear больше не протекает через силуэты;
- contact-вклад гаснет по трём осям: длина ray, глубина камеры (`8…18 м`, за пределом лучи вообще не
  трассируются) и близость точки попадания к краю кадра (блокер там виден лишь частично);
- прямые debug-views показывают оба полных atlas и обе contact masks в правом верхнем углу;
- `dynamic = [ depth_bias ]` принадлежит material; статический raster bias остаётся альтернативой для
  материалов без dynamic state;
- тиры качества заданы specialization-константами шага, а не define'ами материала: `pcf_radius`
  (`draw_scene`), `contact_ray_steps`/`contact_refine_steps` (`build_contact_shadows`);
- overlay показывает caster occupancy каждого atlas region, world-texel/receiver-plane bias, независимые
  AA/PCSS/contact режимы и сглаженные GPU timestamps для directional/spot/depth/contact/forward/blit passes;
- общие free camera, Visage overlay и независимый frame pacer из `../common`.

Сборка и запуск из корня:

```sh
cmake --build build-debug --target PF02_shadows -j 2
./build-debug/subprojects/playgrounds/PF02_shadows/bin/PF02_shadows --validation
```

Управление: `WASD`, `Q/E`, mouse look, `Shift`; `Z/X` — base normal bias, `C/V` — его slope,
`G/H` — receiver-plane scale, `B/N` — raster constant, `M/,` — raster slope, `0/1` — zero/default bias.
`2/3/4` изолируют все/directional/spot lights; `5/6/7` выбирают hard/PCF/Poisson edge AA, `[`/`]`
меняют AA radius. `8` независимо включает spot PCSS, `;`/`'` меняют emitter radius в метрах, `F`
включает contact shadows, `R` целиком отключает sampling shadow maps для A/B, `9` — cascade tint;
`Esc` завершает работу.

CLI-presets `--zero-bias`, `--all-lights`, `--directional-only`, `--spot-only`, `--hard`, `--pcf`,
`--poisson`, `--pcss`, `--contact`, `--no-contact`, `--map-shadows`, `--no-map-shadows`,
`--receiver-plane`, `--no-receiver-plane`, `--cascade-debug` позволяют воспроизвести сравнение.
`--uncapped` отключает только 60 FPS limiter.

## Полный наблюдаемый результат

- четыре stabilized directional cascades в одном atlas с blend bands;
- shadow atlas для spot lights;
- статические и движущиеся casters;
- runtime raw-raster, world-texel normal и receiver-plane bias controls;
- независимые edge AA, spot PCSS и screen-space contact-shadow A/B;
- просмотр обоих atlas и contact masks;
- GPU timings и явная atlas occupancy/culling diagnostics.

Point-light cubemap shadows — отдельное расширение после стабильного первого среза.

## Куда смотреть

Точка входа — `src/main.cpp`; порядок проходов —
`resources/render_config/render_graphs/shadows.tavl`; shadow sampling —
`resources/shaders/shadowed.frag.glsl`, screen-space rays — `contact_shadows.comp.glsl`, а просмотр
atlas/masks — `shadow_debug.frag.glsl`.

## Следующий срез

Порядок зафиксирован 2026-08-17: сначала движковые примитивы, которых физически не хватает выбранным
техникам, затем сами техники — от самых дешёвых по цене/выигрышу к самым дорогим.

1. **Движковые примитивы `libs/painter`** — сделано 2026-08-17, кроме шейдерного consumer'а.
   - `painter::sampler` получил `compare = <compare_op>`: сэмплер становится сравнивающим
     (`compareEnable`/`compareOp`), и шейдер обязан объявить его как `samplerXDShadow`. Для reverse-Z
     нужен `greater_or_equal`.
   - Шаг отрисовки получил `shader_constants = [ name = "value" ]` — specialization constants поверх
     готового SPIR-V. `constant_id`, тип и размер берутся из reflection модуля, поэтому тип значения не
     угадывается по тексту; неизвестное имя — loud error со списком доступных констант. Форма `id_<N>`
     адресует константу без имени напрямую. Важная деталь: `spirv-opt` снимает `OpName`, поэтому карта
     имя→id снимается отдельной сборкой того же исходника с debug info, а pipeline по-прежнему собирается
     из оптимизированного модуля.
   - Живые потребители: `pcf_radius` шага `draw_scene` (заменил define `PF02_PCF_RADIUS`),
     `contact_ray_steps`/`contact_refine_steps` шага `build_contact_shadows`. Размер воркгруппы
     contact-компьюта (`local_size_*_id`) станет следующим потребителем вместе с host-стороной dispatch,
     чтобы не разъехались два источника истины.
   - Шейдерный consumer сделан: sampling идёт через `sampler2DShadow`, PCF взвешен tent'ом, rotated
     Poisson тоже перешёл на сравнивающие taps.
2. ✅ **Комбинирование и upsample вклада contact masks** (2026-08-17): `min` вместо умножения,
   nearest-depth upsample по `.g`-каналу маски, fade по длине ray/глубине камеры/краю кадра.
3. **`guarded contact` preset и A/B против `contact off`** на фиксированных camera bookmarks
   (противоположный угол blocker, grazing receiver, край экрана, вытянутая стенка):
   - шаг ray в screen space фиксированной пиксельной длиной вместо мировых шагов с неравномерной
     проекцией; длина ray — короткая контактная, а не «до источника»;
   - thickness threshold из локальной производной linear depth **в точке блокера** плюс экранный footprint;
   - silhouette rejection: скачок глубины у соседей блокера отклоняет ray, умеренный slope расширяет
     допустимую thickness;
   - backface/`N·L` mask и conservative depth-discontinuity rejection.
4. **Дешёвая оценка полутени.** Один центральный blocker-tap задаёт масштаб радиуса фильтра; сравнить с
   текущим spot-PCSS и с фиксированным радиусом. Цель — впечатление роста мягкости с расстоянием без
   blocker search.
5. **`dual-depth contact`.** Back-depth проход (`cull = front`, reverse-Z ⇒ хранит дальнюю поверхность)
   даёт реальный `[front, back]` интервал вместо эвристики; односторонняя геометрия помечается
   сентинелом и падает на fallback п.3. Сравнить качество, GPU time и память с guarded single-depth;
   альбедо в этот минимальный occlusion G-buffer не включать. Режимы лаборатории:
   `contact off` → `contact guarded` → `contact dual-depth`.
6. Проверить CSM texel snapping и world-texel bias на repeatable camera rail.
7. Добавить conservative directional caster culling и вывести blend-band/stability diagnostics, если
   визуального tint и полного depth atlas окажется недостаточно.
8. После измерений заменить фиксированную раскладку минимальным atlas allocation/lifetime contract.

Bias fixtures, world-texel/receiver-plane correction, independent hard/PCF/Poisson AA, half-resolution
contact masks и CSM baseline уже работают. Текущий spot-PCSS, rotated Poisson и восьмишаговый
screen-space ray — исследовательские presets, а не целевые техники площадки: PCSS остаётся для сравнения
с дешёвой оценкой полутени, Poisson — для сравнения с взвешенным kernel. Stochastic sampling, history,
temporal accumulation, HZB, физическая directional area-light softness и point-light cubemap shadows
намеренно оставлены `PF03`/будущему PBR-срезу.

GPU `complete graph` измеряет интервал от начала первого command buffer до конца present-blit pass на
graphics queue; он намеренно не является временем `vkQueuePresentKHR`, scanout или CPU frame.

## Definition of Done

Движущиеся light и caster дают стабильную объяснимую тень без грубого acne/peter-panning на тестовых
поверхностях; atlas, bias и стоимость доступны в debug UI.

Дополнительно к первому критерию: край тени читается как сглаженный, а не как дизер или лестница, при
неподвижной и при движущейся камере без каких-либо данных предыдущего кадра; интерьер тени сохраняет
ambient; contact-вклад только усиливает затемнение у основания объекта и нигде не создаёт тень там, где
карта теней уверенно говорит «освещено».
