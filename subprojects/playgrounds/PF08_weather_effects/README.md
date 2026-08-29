# PF08 — weather effects

Статус: **срезы 0–4B CLOSED; срез 5 пересмотрен — дешёвая world-map памяти, after-effects и precipitation LOD** (2026-08-29).

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
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=clear --recent-rain=2
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --preset=double_rainbow --weather=clear --recent-rain=2 --surface-time-scale=0 --rainbow-intensity=1.5 --rainbow-saturation=1.6 --rainbow-width=0.85 --rainbow-sharpness=1.25 --rainbow-contrast=0.12 --rainbow-source-balance=1
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=clear --recent-snow=2
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=rain --surface-age=1 --no-precipitation-particles
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=clear --recent-rain=2 --debug=11
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
froxel-среза; штатное значение `3` переиспользует гладкий lighting на тройке соседних срезов, сохраняя все
96 density samples.

История поверхности идёт отдельно от календаря: `--surface-time-scale=F` задаёт мировые секунды на
реальную секунду (по умолчанию `60`), поэтому `pause` останавливает небесную механику, но не осадки вокруг
наблюдателя. `--surface-age=MIN` детерминированно инициализирует карту стартовой погодой, а
`--recent-rain=MM`/`--recent-snow=MM` позволяют войти уже после фронта. `--snow-melt=MMH` и
`--surface-dry-half-life=H` задают простую релаксацию; `--no-surface-weather` скрывает material response и
after-effects, но не останавливает память. Геометрического snow displacement больше нет.

## Граница площадки

Входит:

- одно погодное состояние, которое отдельно описывает облачность, осадки, приземную влажность/дымку,
  ветер и текущую область осадков;
- облачный/туманный объём во froxel-сетке, согласованный с обоими светилами, их тенями и экспозицией;
- near/mid/far проявления дождя и снега, воздействие ветра, укрытия и контакты с поверхностью;
- дешёвая world-space память недавних осадков и минимальный отклик proxy-материалов;
- детерминированные погодные пресеты, A/B-рычаг каждого consumer и измерение GPU-стоимости по проходам.

Не входит: климатическая симуляция, глобальная гидрология, детальные лужи/грязь/мокрые листья, SSR,
generated terrain, production clouds на масштабе целой планеты, gameplay hazards и production art.
Небесная механика задаёт освещение и сезонный контекст, но не притворяется климатической моделью.

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
  ├─ surface memory: recent rain/snow water by coarse world cell
  └─ camera: only lens effects caused by actual precipitation/contact
```

Один важный запрет уже известен из PF07: облачность нельзя изображать только падением яркости. Экспозиция
скомпенсирует скалярное затемнение. Погода обязана иметь геометрическое, пространственное или цветовое
выражение — закрытие дисков и неба, объёмную глубину, движение и след прошедшего фронта.

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
5. **IN PROGRESS — precipitation memory and after-effects.** Один precipitation field связывает
   near/mid/far, слепой дождь, тропический ливень и splash mist. Фиксированная coarse world-map хранит
   недавние rain/snow water values, proxy-материалы реагируют минимально, а радуга и снежное мерцание
   используют эту память. Детальные лужи, грязь, мокрые листья, SSR и lens droplets откладываются до сцены,
   у которой есть соответствующие материалы и геометрические причины.
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

## Пересмотр накопления и поверхности

Первая реализация с одним CPU scalar wetness, процедурными пятнами, GGX-плёнкой и snow displacement была
удалена. Она пыталась угадать свойства отсутствующих материалов и поэтому делала proxy-долину искусственной,
а не убедительно мокрой. PF08 теперь хранит только дешёвый ФАКТ ПРОШЕДШИХ ОСАДКОВ; конкретная игра сама
решит, означает ли он тёмную землю, грязь в колее, капли на листьях, лужи в асфальте или вообще ничего.

`surface_precipitation_memory` — фиксированная world-space карта `64x64` над областью `512x512 м`, то есть
одна ячейка на `8 м`. Каждый `vec4` хранит recent rain reservoir, snow water-equivalent и фильтрованные
текущие rain/snow rates. Compute обновляет всего 4096 ячеек: в каждой интегрирует `rate * duration`, используя
то же advected precipitation field, затем экспоненциально сушит дождевую память; снег отдельно тает, дождь
ускоряет таяние, а талая вода переходит в rain reservoir. Карта привязана к миру, поэтому прошедший фронт
уезжает дальше, а мокрый участок остаётся на месте. Цена прохода на Iris Xe — `0.004–0.007 ms`; одна
буферизованная копия занимает `64 KiB`. `--debug=11` показывает дождевую память красным, снежную голубым.

Отклик намеренно минимален и зависит от material kind. Terrain слегка темнеет от дождя и немного светлеет
от снега; каменная fixture реагирует слабее; foliage почти не меняется. Тот же trajectory-aware roof test
сохраняет область под видимым навесом сухой. Удалены случайные puddle masks, отдельный wet GGX, отражение
sky LUT и геометрическое поднятие вершин: без roughness/material maps, микрорельефа, листовой воды и SSR это
были дорогие утверждения о мире, которого в сцене нет. В 120-кадровом A/B clear/recent-rain/recent-snow
scene minima равны `1.336/1.377/1.353 ms`, total `5.783/5.845/5.799 ms` — остаточный отклик практически
теряется в разбросе кадра.

Все authored-пресеты выбирают дождь ИЛИ снег. Плавный переход может коротко содержать оба типа: это и
устраняет pop, и соответствует узкой области мокрого снега. Настоящая production-система должна выбирать
фазу по температуре влажного термометра, сезону и биому; PF08 не изобретает климатическую модель из имени
пресета. Этот контракт теперь входит в `--verify`.

## Объём осадков и after-effects

Осадки имеют три представления. Near — persistent pool из `4096` stable slots с depth contacts. Mid draw
процедурно строит `8192` кандидатов в двух world-anchored слоях сетки `64x64`, без history, collision и CPU
update. Rain по умолчанию доходит до `120 м`, snow — до `160 м`; дальше работает `160x90x96` froxel volume.
Billboard'ы затухают на обоих стыках, поэтому это непрерывный LOD статистического поля, а не попытка
нарисовать каждую далёкую каплю.

Исправлен конкретный snow failure: startup age теперь меняет только вертикальную фазу хлопьев. XZ сразу
заполняет текущий camera-local объём и заворачивается у уже затухающей внешней границы; медленный снег больше
не сносится целиком на `40–60 м`, оставляя камеру наблюдать небольшой emitter со стороны.

Near, mid, far и surface-memory читают один `pf08_precipitation_field_density`: coverage, размер ячейки,
мягкость края и world-XZ advection. Внутри удалённой ячейки объекты теряют контраст по Beer–Lambert, её край
читается как стена дождя, а после её прохода карта поверхности остаётся неизменной. `sunshower` даёт
`3.5 мм/ч` при открытом солнце и почти не режет дальность. `downpour` даёт `60 мм/ч`, низкий облачный слой,
сильный optical column и метровый splash-mist volume; его цена не растёт с числом камней или травинок.

Два after-effect используют уже существующие данные. После дождя при открытом низком основном светиле небо
строит primary rainbow вокруг anti-solar direction с правильным порядком красный→синий; сильный текущий дождь
его подавляет, поэтому карта памяти открывает короткое окно после прохода фронта. Первая физически сдержанная
версия оказалась различима только в аналитическом A/B, поэтому shipped-дуга намеренно художественная: полосы
расширены примерно до `0.7°`, вокруг них добавлена мягкая светлая дуга `1.55°`, а radiance имеет явно
задокументированный visibility scale `0.0065`. Геометрия и условия появления остались прежними, но fixed
after-rain против dry теперь даёт `MAE 0.00431119`, и радуга уверенно читается без difference-image. Снег
получает редкие узкие блики: одна стабильная случайная микрогрань на world-cell `12.5 см`, освещённая уже
посчитанным direct primary light. Это даёт мелкое солнечное мерцание без normal map, дополнительных частиц и
второго shadow/light loop.

### Художественный контракт радуги

Физика отвечает за anti-solar geometry и базовые углы `41–42.5°`; проект отвечает за читаемость. Параметры
упакованы в три отдельных GPU-вектора, а не спрятаны в свободных компонентах unrelated-настроек:

| CLI | Default | Смысл |
|---|---:|---|
| `--rainbow-intensity` | `1.0` | HDR radiance, допустимо до `16` |
| `--rainbow-saturation` | `1.15` | насыщенность спектра после разделения полос |
| `--rainbow-width` | `1.0` | общий множитель ширины primary/secondary/veil |
| `--rainbow-sharpness` | `1.0` | показатель профиля: выше — чётче края |
| `--rainbow-veil` | `1.0` | мягкая светлая дуга вокруг спектра |
| `--rainbow-contrast` | `0.08` | локальное затемнение Alexander's band; не ещё одна добавка света |
| `--rainbow-persistence` | `1.0` | чувствительность к стареющей rain-memory, не скорость высыхания карты |
| `--rainbow-rain-cutoff` | `8 мм/ч` | интенсивность текущего дождя, полностью скрывающая дугу |
| `--rainbow-sources` | `all` | `primary`, `brightest` или все видимые светила |
| `--rainbow-source-balance` | `0.65` | `0` — физическое отношение lux, `1` — равная яркость дуг |
| `--rainbow-separation` | `1.0` | только художественное расстояние центров; светила не двигает |
| `--rainbow-secondary` | `0.0` | вторичный метеорологический порядок около `51°` с обратными цветами |

`double_rainbow` фиксирует г1 д0 16:00: Aurin стоит на `12.79°/240.25°`, Ember на
`23.86°/231.88°`, угловое разделение около `13.7°`. Поэтому при физическом separation `1` видны две
самостоятельные PRIMARY-дуги: верхняя от Aurin и нижняя от Ember. Это не обычная primary+secondary rainbow;
последняя включается отдельно и при двух светилах создаёт уже четыре спектральные дуги.

В художественном кадре `intensity=1.5`, `saturation=1.6`, `width=0.85`, `sharpness=1.25`, contrast `0.12`
и равные источники дают `MAE 0.00714818` против dry; `all` против `primary` — `0.00104737`, то есть вклад
Ember измерим и явно виден. Iris Xe, 120 кадров: dry sky/total minima `0.989/5.802 ms`, две активные дуги
`1.278/6.224 ms`; редкий двойной after-effect стоит примерно `0.29 ms` в sky pass, а сухой early-out
не платит за цикл источников.

Карта rain-memory имеет фиксированный world-space размер и должна переживать resize/fullscreen. Найденный
engine bug был именно здесь: `resize_viewport` пересоздавал только screensize-ресурсы, но обнулял
всю temporal-историю, включая эту карту. В итоге радуга исчезала не от смены aspect/FOV, а от
потери своего погодного условия. Теперь resize очищает только реально пересозданную screensize history;
динамический тест `1280x720 -> 1600x900` на 20-м кадре сохранил обе дуги на итоговом 80-м.

Density всё ещё вычисляется во всех 96 Z-slices, но precipitation lighting по умолчанию переиспользуется на
трёх соседних срезах. На `downpour` stride `2 -> 3` снизил volume minimum `2.883 -> 2.405 ms`, total
`9.257 -> 8.622 ms`; фиксированный A/B дал `MAE 0.00203176`, `PSNR 47.15 dB`, визуальной ступенчатости не
видно. `--precip-light-stride=1` остаётся точным reference path.

Release build, runtime clear/recent-rain/recent-snow/downpour и Vulkan validation проходят без VUID/API
warning/error; `--verify` — `98/98`. Шесть тестов удалённого CPU scalar-интегратора также удалены, а новый
контракт authored rain XOR snow добавлен. PF07, `compare_pf07_baseline.sh` и frozen reference frames при этом
пересмотре не запускались.
