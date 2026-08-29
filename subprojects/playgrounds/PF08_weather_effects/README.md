# PF08 — weather effects

Статус: **срезы 0–5B CLOSED; 5B — масштаб осадков, погодный контекст и performance** (2026-08-29).

PF08 проверяет погоду как состояние открытого мира, а не как отдельный дождевой emitter. Площадка начинает
с независимой копии закрытого `PF07_party_environment`: та же P-type двойная система, календарь и затмения,
физическая атмосфера и экспозиция, две системы каскадных теней, proxy-долина и `6000` кустов, читающих одно
ветровое поле. PF07 остаётся неизменяемой контрольной стороной; вся дальнейшая погодная работа принадлежит
PF08 и не создаёт CMake/source dependency между лабораториями.

## Запуск

```sh
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --verify
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --preset=noon --weather=haze
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=fog
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=cloudy
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=overcast --debug=10
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=rain
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=sunshower
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=downpour --surface-age=1
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=rain --no-rain-particles
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=snow --surface-age=30
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=rain --surface-age=1 --no-precipitation-particles
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=fog --debug=8
./subprojects/playgrounds/PF08_weather_effects/compare_pf07_baseline.sh
```

`--weather=clear|haze|windy|fog|cloudy|overcast|sunshower|rain|downpour|snow` выбирает состояние сразу,
`T` циклически запускает переход длительностью
`--weather-transition=S` (по умолчанию `4 s`). `--turbidity`, `--wind`, `--wind-direction` сохранены как
независимые overrides поверх пресета; локальная среда отдельно управляется через `--fog-extinction=`,
`--fog-albedo=`, `--fog-anisotropy=` и `--fog-range=`. Ими изолируется один consumer для A/B.
Профиль отдельно задаётся `--fog-base=` и `--fog-height=` (экспоненциальный scale height), пространственное
поле — `--fog-variation=`, `--fog-cell=` и `--fog-speed=`.
Параметры дождя изолируются через `--rain-rate=`, `--rain-speed=`, `--rain-wind=`, `--rain-length=`,
`--rain-radius=`, `--rain-extinction=` и `--rain-range=`. `--no-rain-particles` оставляет только far volume,
`--no-rain-collision` — streaks без surface contacts. Четыре астрономических пресета PF07 сохранены
буквально. `compare_pf07_baseline.sh` запускает ТОЛЬКО PF08 и
сравнивает его с четырьмя уже зафиксированными PNG PF07; сам PF07 повторно запускается только при явном
обновлении его `reference_frames/`.

Общее пространственное поле осадков настраивается через `--precip-coverage=`, `--precip-cell=` и
`--precip-speed=`; его одновременно читают near spawn, процедурный mid LOD и far extinction. Границы
particle LOD задаются `--rain-mid-radius=`/`--snow-mid-radius=`, приземная водяная взвесь —
`--splash-mist=`/`--splash-height=`. `--precip-light-stride=1` включает точный A/B lighting каждого
froxel-среза; штатное значение `2` переиспользует гладкий lighting на соседней паре, сохраняя все 96
density samples.

История поверхности идёт отдельно от календаря: `--surface-time-scale=F` задаёт мировые секунды на
реальную секунду (по умолчанию `60`), поэтому `pause` останавливает небесную механику, но не снегопад вокруг
наблюдателя. `--surface-age=MIN` детерминированно прогревает историю на заданное число МИРОВЫХ минут и
нужен для воспроизводимых A/B; `--snow-melt=MMH`, `--surface-dry-half-life=H`,
`--no-surface-weather` и `--no-snow-displacement` разделяют интегратор, материал и геометрию.

## Граница площадки

Входит:

- одно погодное состояние, которое отдельно описывает облачность, осадки, приземную влажность/дымку,
  ветер и накопленную мокроту поверхности;
- облачный/туманный объём во froxel-сетке, согласованный с обоими светилами, их тенями и экспозицией;
- near/mid/far проявления дождя и снега, воздействие ветра, укрытия и контакты с поверхностью;
- мокрые материалы, лужи/рябь и только те screen-space эффекты, у которых есть world-space причина;
- детерминированные погодные пресеты, A/B-рычаг каждого consumer и измерение GPU-стоимости по проходам.

Не входит: климатическая симуляция, глобальная гидрология, generated terrain, production clouds на масштабе
целой планеты, gameplay hazards и production art. Небесная механика задаёт освещение и сезонный контекст,
но не притворяется климатической моделью.

## Главный контракт

Погода не должна быть enum `clear|rain|snow`. Такой enum быстро расходится по веткам: небо решает одно,
растительность другое, частицы третье, а мокрые поверхности остаются в прежнем состоянии. Источник истины —
небольшой непрерывный weather state; именованные пресеты только заполняют его. Каждый эффект читает нужную
часть состояния:

```text
weather state
  ├─ atmosphere/LUT invalidation: aerosol, humidity
  ├─ froxel medium: cloud/fog density, advection, light visibility
  ├─ foliage + precipitation: shared wind field
  ├─ surfaces: accumulated wetness, puddles and ripples
  └─ camera: only lens effects caused by actual precipitation/contact
```

Один важный запрет уже известен из PF07: облачность нельзя изображать только падением яркости. Экспозиция
скомпенсирует скалярное затемнение. Погода обязана иметь геометрическое, пространственное или цветовое
выражение — закрытие дисков и неба, объёмную глубину, движение, мокрый отклик поверхности.

## Срезы

0. **DONE — frozen clear baseline.** Независимый `PF08_weather_effects`, собственные namespace/resources,
   `41/41`, побитное сравнение четырёх clear-кадров с PF07 и Vulkan validation без API-сообщений.
1. **DONE — weather state.** Данные и пресеты `clear|haze|windy`, переходы без перестройки render graph,
   общее ветровое поле, CLI/runtime-контроли и диагностическая визуализация значений. Пока consumer не
   существует, поле в state не добавляется «на будущее»: `overcast`, `rain`, `snow` были добавлены только
   одновременно с настоящими froxel/particle consumers.
2. **DONE — froxel medium.** Проверяемый однородный интеграл как первый proof, затем общий экспоненциальный
   высотный профиль и advected world-space density field для объёма и поверхностей; оба светила, луны,
   атмосферное прохождение и shadow visibility.
3. **DONE — finite cloud layer.** `cloudy|overcast`, 3D world-space density, общий ветер, два светила,
   self-shadowing и совпадающая с объёмом движущаяся тень на поверхности.
4. **DONE — precipitation across distance.** 4A закрывает дождь: near drops, mid/far extinction и
   depth-driven impact-события. 4B добавляет снег в тот же persistent pool и настоящий видимый навес,
   чья геометрия одновременно задаёт near collision и сухой объём в far medium.
5. **IN PROGRESS — wet world and screen manifestations.** 5A закрывает snowpack, таяние/высыхание,
   world-space coverage, геометрическую толщину и albedo/roughness/specular response. 5B расширяет один
   precipitation field на near/mid/far, добавляет слепой дождь, тропический ливень, splash mist и снимает
   главный performance-регресс wet lighting. Persistent локальная карта истории, лужи, рябь и lens
   droplets только как следствие попадания воды остаются следующими частями, с отдельными A/B.
6. **Закрывающий аудит.** Фиксированные clear/overcast/rain/snow кадры в нескольких временах суток,
   временные переходы, GPU budget и Vulkan validation. Побитный PF07 baseline остаётся историческим gate
   срезов 0–4A: с 4B PF08 намеренно содержит новую постоянную геометрию навеса.

## Definition of Done нулевого среза

- PF08 собирается отдельным target и не линкуется с PF07;
- `--verify` сохраняет все `41/41` инварианта небесной системы;
- четыре фиксированных clear-state PPM побитно совпадают с PF07;
- окно PF08 проходит Vulkan validation без `VUID`, warning или error от API-проверок;
- назначение погодных срезов записано здесь до появления первой реализации.

Все пять gates закрыты 2026-08-28. Release target собирается независимо; `--verify` даёт `41/41`;
`compare_pf07_baseline.sh` возвращает `MATCH` для `noon`, `double_sunset`, `night`, `eclipse`; восьмикадровый
`--validation`-запуск не выдаёт `VUID`, warning или error от API-слоя. Каталог pipeline cache создаётся
post-build, поэтому первый запуск нового target не оставляет отдельный filesystem warning.

## Что закрыл срез 1

`src/weather.*` — чистая от Vulkan модель. Она загружает authored-точки из
`resources/weather/presets.tavl`, проверяет пустые/повторные имена и физически бессмысленные значения,
нормализует направление и интерполирует его по кратчайшей дуге: середина `350° -> 10°` равна `0°`, а не
`180°`. Переход идёт в реальных секундах и хранит исходный snapshot; новый выбор посреди перехода стартует
из текущего показанного состояния без щелчка.

После среза 2 в state шесть величин с тремя настоящими consumer'ами:

- `aerosol_turbidity` подставляется в единую модель атмосферы и инвалидирует зависящий от среды LUT cache;
- `wind_direction_deg` и `wind_strength_m` один раз упаковываются в shared GPU block, который читают и
  `scene.vert`, и `shadow.vert`. Поэтому куст и его тень не могут получить разные погодные кадры.
- `fog_extinction_per_m`, `fog_scattering_albedo`, `fog_anisotropy` описывают локальную среду и читаются
  froxel-pass; нулевой extinction остаётся точным clear-state.

Наблюдаемость измерена на одном `noon`-кадре при фиксированном EV. Только аэрозоль (`1.0 -> 2.4`) при
clear-ветре меняет `16507.3` pixel-equivalent; только ветер (`250°/0.22 -> 320°/0.55`) при clear-аэрозоле —
`8629.18`. Default `clear` после подключения state снова дал четыре `MATCH` против PF07. `--verify` вырос с
`41/41` до `52/52`: добавлены разбор authored-конфига, точные baseline-значения, круговая интерполяция,
реальные секунды перехода, прерывание без щелчка и точное прибытие в authored-state. Non-clear `windy`
проходит Vulkan validation без `VUID`, warning или error от API-слоя.

## Что закрыл срез 2

Локальный туман начинал с настоящего `160x90x32` 3D-образа RGBA16F, а не полноэкранного шума; после
облачного среза общий объём имеет 96 дальностных слоёв. Экранные оси редкие, ось расстояния квадратична;
каждая compute-нить проходит луч один раз и пишет нарастающие
`in_scattering.rgb` и `transmittance.a`. Нулевой слой хранит точное `(0, 1)`, поэтому ближайшая геометрия
не получает целый первый слой тумана. На каждом сегменте Beer–Lambert и рассеяние интегрируются
аналитически: `T=exp(-sigma_t*d)`, `S=L_source*albedo*(1-T)`, а не приближением `sigma*d`.

Источник света не придуман погодой отдельно: объём читает обе звезды и луны из общего `sky_buffer`,
атмосферное прохождение — из общего LUT, тени — из тех же двух наборов каскадов. Для воздуха используется
одна shadow-выборка вместо surface PCF 3x3: сама froxel-сетка затем фильтруется трилинейно, и второй
девятиточечный фильтр только девятикратно увеличил бы цену. Изотропная часть берётся из пяти выборок уже
посчитанного `sky_view_lut`; направленная использует нормированную фазу Henyey–Greenstein.

Композиция `L_out=L_scene*T+S` идёт в отдельный HDR `weather_color` после сцены, но ДО экспонометра.
Иначе замер видел бы ясную сцену, а вывод — туманную. Глубина reverse-Z ограничивает интеграл на предметах,
небо проходит полную локальную дальность. При нулевом extinction fragment-pass берёт прямую ветку копирования
без выборки froxel-образа: после добавления обоих проходов все четыре clear-кадра снова `MATCH` PF07 побайтно.

Численный GPU debug `--debug=8` показывает `T` напрямую. После высотного follow-up горизонтальный луч с
камеры `2 m` в гладком A/B authored `fog` (`sigma_t=0.018 1/m`, `H=240 m`, дальность `220 m`, variation 0)
даёт `5/255 = 0.01961`; аналитика требует `exp(-0.018 * exp(-2/240) * 220)=0.01970`, то есть тот же
результат после 8-bit округления дампа. CPU-проверки выросли до `64/64`. На Iris Xe, минимум из 76
steady-state кадров 1280x720: clear volume/apply `0.127/0.224 ms`, активный ячеистый fog
`0.619/0.279 ms`. Отдельную цену noise честно измерить не удалось: соседние smooth/cells запуски дали
total minimum `6.572/6.493 ms`, то есть знак разницы утонул в run-to-run spread. Активный `fog` с
debug-view прошёл Vulkan validation без `VUID`, warning или error от API-слоя.

FOLLOW-UP ПО ТЕНЯМ. Shadow map — только бинарная/фильтрованная видимость светила; сама по себе она не
знает, сколько среды лежит между источником и точкой. Поэтому общий `pf08_local_medium.glsl` теперь даёт
и froxel-pass, и surface-lighting один экспоненциальный профиль. Поверхность получает
`direct * fog_light_transmittance * shadow_visibility`, а объём тем же visibility вырезает прямое
in-scattering. В результате тень на земле исчезает вместе с прямым светом, но слабая объёмная тень может
остаться как физический световой столб. Измерение нижней половины одного noon-кадра, A/B `2` против `0`
shadow sources: clear `MAE 0.00203056`, authored fog (`H=240 m`, variation `0.35`) `0.00022983` — контраст
подавлен на `88.7%`.

FOLLOW-UP ПО НЕОДНОРОДНОСТИ. Две дешёвые value-noise октавы живут в мировых `XZ`, а не в UV кадра,
вытянуты в вертикальные столбы и сдвигаются тем же направлением ветра, что листва. Скорость отдельна,
потому что advection измеряется в `m/s`, а сила качания куста — в метрах отклонения верхушки. Столбовая
модель сохраняет аналитический light column на surface-pass; один и тот же column modulation вычисляется
ровно один раз на froxel/fragment и переиспользуется всеми светилами. На paused debug-кадрах без листвы
frame 8 против 80 при `fog-speed=0` побитно совпадает (`MAE 0`), authored speed даёт `MAE 0.000631`.
Стандартное отклонение `T` вдоль одного небесного ряда: гладкий профиль `0.00193`, ячеистый `0.01891`,
то есть пространственный сигнал почти в десять раз сильнее фонового изменения направления луча.

## Что закрыл срез 3

Облако не переиспользует бесконечный экспоненциальный fog-профиль. Это отдельный конечный слой с authored
`base/top`, coverage, extinction, scattering albedo и HG anisotropy. `cloudy` оставляет разорванные тела,
`overcast` собирает низкий сплошной покров. Три октавы 3D value noise живут в мировых координатах; XZ
переносится тем же направлением ветра, что туман и листва, но собственной скоростью в м/с. Вертикальный
`sin²`-профиль гладко сводит плотность к нулю на обеих границах слоя.

Тот же `pf08_clouds.glsl` читают froxel-pass и surface lighting. Для тени на земле вертикальный интеграл
`sin²` аналитический: полный столб равен половине толщины слоя, а 3D-модуляция берётся в середине луча
через облако. Поэтому облако и его тень не являются двумя независимо движущимися noise-текстурами.
`--debug=9` показывает density field, `--debug=10` — только Beer–Lambert transmittance главного светила
к поверхности. На paused кадрах 8→80 authored advection меняет `cloudy` density field на `MAE 0.00239`,
а `overcast` cloud shadow — на `0.000842`; при `--cloud-speed=0` обе пары побитно совпадают.

Общий объём вырос до `160x90x96`: 48 дальностных слоёв давали видимые горизонтальные ступени на границе
километрового облака, 96 их убрали. На Iris Xe в соседних 80-кадровых 1280x720 запусках minimum
volume/apply/total: clear после оптимизации пустого прохода `0.009/0.209/5.783 ms`, cloudy
`1.235/0.363/7.404 ms`, overcast `1.248/0.369/7.569 ms`. `cloudy` проходит Vulkan validation без VUID,
API warning или error. Headless contract — `72/72`.

## Что закрыл срез 4A

`rain` — одно непрерывное состояние, а не screen overlay. `rain_rate_mm_h` управляет долей активных slots
в фиксированном pool из `4096` записей; скорость падения, горизонтальный перенос общим направлением ветра,
длина streak и near-radius размерены в метрах и м/с. Один compute invocation владеет одним стабильным slot,
current copy пишется на GPU, `history=1` читается как прошлый кадр — CPU не раздаёт капли и не читает их назад.
Первые четыре кадра детерминированно рассеивают pool по camera-local объёму, затем движение идёт с фиксированным
`1/60 s` в кадровых тестах. Важная найденная ошибка: `wind_params.xy` означает world `XZ`; сборка его как
`vec3(vec2, fall_speed)` отправляла падение в `Z` и делала почти горизонтальный дождь. Теперь компоненты
переводятся явно в `(wind.x, -fall, wind.z)`.

Контакт — не случайный splash: compute стоит после opaque scene, берёт depth этого же кадра, пересекает
отрезок движения капли с реконструированной поверхностью и на `0.22 s` превращает тот же slot в расширяющееся
кольцо. Streak/ring вручную сравнивается с reverse-Z depth при композиции, поэтому осадки не просвечивают через
опорную геометрию. Это намеренно только контакт с ВИДИМОЙ поверхностью. У PF07 fixture нет физической крыши;
вводить невидимый AABB как «укрытие» означало бы создать gameplay collider без визуальной причины. Укрытие
придёт в 4B вместе с видимым навесом/объёмом, который сможет отсекать rain spawn и far medium согласованно.

За near-radius дождь переходит в тот же `160x90x96` froxel volume: cubic smoothstep начинается на `0.72R`
и за `0.45R` набирает authored `rain_far_extinction_per_m`. Вертикально дождь существует от поверхности до
основания облака; источники рассеяния — те же две звезды, луны и sky ambient, что у fog/cloud. Near streaks
композятся в HDR после fog apply, но до metering, поэтому экспозиция видит конечный погодный кадр.

Проверка разделёнными A/B при fixed noon EV: particles против `--no-rain-particles` дают `MAE 0.000300492`;
collision против `--no-rain-collision` — `0.0000308103`, причём difference-image локализует кольца на земле;
far extinction против `--rain-extinction=0` при выключенных particles — `0.0115107`. Дождевой light-column
также ослабляет прямой свет на поверхности и к froxel scattering, как fog/cloud. На Iris Xe минимум
steady 1280x720 для финального authored rain: simulation `0.036 ms`, volume `3.772`, apply `0.334`,
rain draw `0.038`, total `9.750 ms`. `--verify` теперь `86/86`, rain Vulkan-validation clean. Единственный
финальный набор clear кадров снова дал `AE=0` для всех четырёх сохранённых PF07 PNG; PF07 не запускался.

FOLLOW-UP ПО RUNTIME-ПЕРЕХОДУ. `T` впервые обнаружил контракт, которого fixed preset не касался:
`multiscatter_lut` хранит две cache-копии, а плавная turbidity меняется каждый кадр. Старый код поэтому
двигал `atmosphere_cache` на соседних кадрах и `painter` правильно останавливал процесс, пока GPU ещё мог
читать перезаписываемую копию. Новый host gate разрешает rebuild только через два submitted frame. Сам
weather state, transmittance и остальные consumers остаются покадровыми; multiscatter отстаёт максимум на
один кадр, а финальное dirty-значение обязательно запекается после окончания перехода. Четыре headless
проверки фиксируют ритм `build/skip/build/skip` и подняли контракт до `86/86`.

## Что закрыл срез 4B

`snow` — восьмой authored weather preset и второй тип в том же pool из `4096` стабильных slots. Это не
перекрашенный дождь: rate хранится как водный эквивалент мм/ч, скорость падения `1.6 м/с`, общий ветровой
перенос `3.2 м/с`, размер хлопья `4.5 см`, near-radius `22 м`, far extinction `0.00085 1/м`. Четвёртый
`vec4` particle record хранит тип и постоянную фазу; снег рисуется вращающимся мягким хлопком, медленно
падает и получает поперечный flutter. Дождь и снег могут одновременно занимать детерминированные доли
pool во время непрерывного перехода, без нового enum pipeline и без CPU readback. Контакт снега завершает
хлопок без дождевого splash; накопление покрытия реализовано следующим wet-world срезом 5A ниже.

Укрытие теперь имеет видимую причину: к fixture добавлены одна крыша и четыре стойки. `make_fixture_scene`
возвращает инстансы и AABB крыши вместе, поэтому render, near segment collision и froxel shelter не имеют
трёх независимо authored границ. Near particle сначала пересекает этот world-space AABB и лишь затем
пытается использовать current screen depth. Для far snow/rain точка сухая, если луч вверх ПРОТИВ реальной
скорости падения пересекает нижнюю плоскость крыши; следовательно, косой снег образует подветренную границу
и может задуваться через открытый бок. `--no-shelter-occlusion` оставляет крышу видимой и отключает только
этот физический consumer для чистого A/B.

Хлопья становятся sub-pixel раньше дождевых streaks, поэтому snow froxel handover начинается на `0.25R`
и набирает полный вес за `0.45R`; дождевой контракт `0.72R/0.45R` не менялся. Far снег имеет собственное
extinction, альбедо `0.98` и HG `g=0.45`, но использует те же звёзды, луны, sky ambient и precipitation
light-column, что дождь. Fixed-noon A/B: near pool против `--no-precipitation-particles` даёт
`MAE 0.000349426`, far extinction против `--snow-extinction=0` — `0.0167005`, shelter on/off —
`0.000073345`; shelter difference-image локализован под крышей и вдоль её подветренного края.

Release build и runtime GLSL compile проходят; headless contract — `93/93`. Финальный восьмикадровый snow
запуск с Vulkan validation не выдаёт VUID/API warning/error. На Iris Xe 1280x720 его minima:
simulation `0.023 ms`, volume `3.703`, apply `0.331`, particle draw `0.045`, total `10.330 ms`. PF07 не
запускался и frozen PNG не переснимались. Новая постоянная крыша осознанно завершает эпоху побитного clear
совпадения с PF07 после 4A: скрывать физическое укрытие только в clear означало бы заставить мир менять
геометрию при смене погоды.

## Что закрыл срез 5A

`src/surface_weather.*` хранит историю, а не текущее имя пресета: водный эквивалент снега в миллиметрах
и безразмерную мокроту. При штатной шкале `60` одна реальная минута равна одному мировому часу; authored
снег `7 мм/ч` за это время даёт `7 мм` воды или `70 мм` рыхлого снега при явном отношении `10:1`.
Толщина ограничена `12 см`. Когда снегопад прекращается, pack тает со скоростью `0.8 мм воды/ч`, дождь
ускоряет таяние, а получившаяся вода входит в тот же wetness. Намокание и высыхание экспоненциальны,
поэтому один шаг `60 s` и шестьдесят шагов `1 s` дают один результат; dry half-life по умолчанию `0.35 h`.
Температуры здесь намеренно нет: выводить её из слова `snow` было бы скрытой климатической моделью.

CPU передаёт только глобальные depth/coverage/wetness. Пространство строится в общем
`pf08_surface_weather.glsl`: две low-frequency value-noise октавы в world `XZ`, authored 12-метровая
ячейка, вес наклона по нормали и тот же луч против реальной скорости осадков, которым 4B проверяет крышу.
Поэтому ранний покров появляется пятнами, склоны держат меньше снега, а под видимым навесом остаётся сухое
место с подветренной границей. Размер пятен сначала был `2–5 м`, но попал на трёхметровую сетку долины и
дал полосы; масштаб `4–12 м` выбран как минимальный, который текущая геометрия действительно разрешает.

Снег — не только перекраска. `scene.vert` поднимает открытую поверхность вертикально по гравитации на
накопленную толщину, а `shadow.vert` вызывает ТОТ ЖЕ helper после того же wind deformation. Поэтому
покрытая плита, её силуэт и отбрасываемая тень не расходятся. Листва не раздувается белой оболочкой:
трава остаётся геометрией, торчащей из pack. Fragment material переводит снег в неоднородное холодное
альбедо `0.72..0.97`; жидкая плёнка затемняет исходное альбедо до `58%`, снижает roughness `0.72 -> 0.22`
и включает GGX с `F0=0.045`. Небо отражается из существующего sky-view LUT. У него нет prefiltered mip
chain, поэтому один резкий sample явно ослаблен коэффициентом `0.25`; без этого мокрый склон на скользящем
угле становился бело-голубым зеркалом и читался как снег. Дорогой второй обход светил выполняется только
для жидкой плёнки: матовый снег выражается diffuse, пятнами и толщиной.

Fixed-noon кадры сняты только PF08, с выключенными near particles и far precipitation extinction.
Возраст `0 -> 5` мировых минут (`0 -> 5.8 мм`, coverage `0 -> 52%`) даёт `MAE 0.019257`; `5 -> 30`
(`5.8 -> 35 мм`, coverage `52 -> 99%`) — `0.0207413`. Shelter on/off локализован под крышей и даёт
`0.00011922`; displacement on/off при `35 мм` даёт `0.000881147`, с разностью на рельефе, верхах плит
и их тенях. Rain age `1 min` против `--no-surface-weather` даёт `0.0208997`: открытый грунт темнеет и
ловит отражение неба, защищённый остаётся сухим.

Release build и runtime GLSL проходят; `--verify` — `99/99`, включая накопление, frame partition,
water-to-depth, таяние, передачу талой воды и dry half-life. Снег и дождь Vulkan-validation clean. Iris Xe
1280x720 steady minima для scene/total: clear `1.194/5.534 ms`, снег `1.553/9.496`, мокрый дождевой кадр
`3.040/11.052`; totals включают разные authored fog/cloud states, поэтому изолированная цена material видна
именно в scene pass. PF07 и `compare_pf07_baseline.sh` не запускались, frozen PNG не переснимались.

Оставшаяся честная граница: история пока глобальная, а shelter/slope/noise mask вычисляется заново из
текущей геометрии — это не persistent footprint texture и не гидрология. Лужи, течение, рябь от impacts и
lens droplets остаются продолжением среза 5, а не притворяются уже решёнными одним scalar wetness.

## Что закрыл срез 5B

ОСАДКИ ТЕПЕРЬ ИМЕЮТ ТРИ ПРЕДСТАВЛЕНИЯ, а не одну маленькую коробку возле камеры. Near остаётся
persistent pool из `4096` stable slots с depth contacts. Новый mid draw процедурно строит `8192` кандидатов
в двух world-anchored слоях сетки `64x64`: у них нет SSBO history, collision и CPU update, а при движении
камеры меняется только внешний ряд world cells. Rain по умолчанию доходит до `120 м`, snow — до `160 м`,
после чего непрерывность берёт `160x90x96` froxel volume. Billboard'ы становятся немного крупнее с
дальностью и одновременно затухают на обоих стыках; это LOD видимого статистического поля, а не попытка
нарисовать каждую далёкую каплю.

Исправлен конкретный snow failure. Startup раньше создавал XZ возле камеры, а затем сразу прибавлял ко всей
позиции `velocity * random_age`. Медленный хлопок живёт долго и успевал сместиться на `40–60 м` при near
radius `22 м`, поэтому камера видела снегопад со стороны. Теперь startup age меняет только вертикальную
фазу, а XZ сразу заполняет текущий объём. Во время жизни camera-local XZ torus переукладывает хлопья у уже
затухающей внешней границы; весь pool больше не выдувается в одну сторону.

Near, mid и far читают ОДНО `pf08_precipitation_field_density`: coverage, размер ячейки, мягкость края и
advection в world XZ вдоль общего направления ветра, но с собственной скоростью м/с. Поэтому distant rain
не равномерен по экрану: внутри moving cell объекты теряют контраст по Beer–Lambert и её кромка становится
той самой дождевой стеной; за пределом той же ячейки не рождаются ни near drops, ни mid streaks. Coverage
`1` возвращает точную единицу без noise cost и сохраняет прежние равномерные `rain|snow`. Это ТЕКУЩЕЕ поле
погоды, не карта прошлого накопления: умножать им старый снег означало бы заставить уже выпавший pack уехать
вместе с тучей, поэтому persistent surface clipmap оставлен отдельной будущей задачей.

Погода вокруг дождя теперь authored явно. `sunshower` даёт `3.5 мм/ч` в разорванных ячейках при открытом
солнце, почти нулевой far extinction и без общего влажного fog. `downpour` даёт `60 мм/ч`, низкий облачный
слой, локальные ячейки, сильный optical column и метровый приземный слой splash mist. Взвесь не состоит из
новых impact particles: это низкий экспоненциальный volume, умноженный на rain rate и то же precipitation
field, поэтому её цена не растёт с числом видимых камней или травинок.

PERFORMANCE FOLLOW-UP разделил ошибочно объединённые причины. CPU history оказался пренебрежимо дешёвым;
снег с настоящим displacement добавляет около `0.13 ms` scene pass. Главный regression мокрого кадра был
в шейдере: diffuse lighting уже обходил все звёзды/луны и shadow maps, затем wet GGX повторял тот же обход.
`pf08_surface_wet_radiance` теперь за один цикл считает diffuse и specular, совместно используя atmosphere,
cloud/fog/precipitation transmittance и shadow visibility. Fixed rain A/B: dry scene minimum `1.445 ms`, wet
`1.684 ms`, то есть surcharge `0.239 ms` вместо прежних `1.617 ms` — снято около `85%`; контрольный wet
кадр до/после merge побитно одинаков.

Вторая оптимизация не возвращает прежние cloud stairs: density по-прежнему вычисляется во всех 96 Z-slices,
но при активных осадках гладкие source visibility/light transmittance переиспользуются на соседней паре.
На authored `downpour` точный `--precip-light-stride=1` дал volume minimum `4.332 ms`, штатный stride 2 —
`2.890 ms`; total `10.712 -> 9.272 ms`. Fixed A/B имеет `MAE 0.00252273`, `PSNR 45.31 dB`, а рядом кадры
визуально неразличимы. Mid draw стоит около `0.10 ms`, simulation обычно `0.03 ms`.

Release build, runtime GLSL, `git diff --check` и Vulkan validation для `downpour` проходят без VUID/API
warning/error. Headless contract теперь `103/103`, включая два новых пресета и их различия по облачности,
видимости, splash mist и spatial coverage. PF07, `compare_pf07_baseline.sh` и frozen reference frames в этом
follow-up не запускались.
