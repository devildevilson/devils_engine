# PF08 — weather effects

Статус: **срезы 0–3 CLOSED — fog/froxel-среда и конечный облачный слой зафиксированы** (2026-08-28).

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
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --weather=fog --debug=8
./subprojects/playgrounds/PF08_weather_effects/compare_pf07_baseline.sh
```

`--weather=clear|haze|windy|fog|cloudy|overcast` выбирает состояние сразу, `T` циклически запускает переход длительностью
`--weather-transition=S` (по умолчанию `4 s`). `--turbidity`, `--wind`, `--wind-direction` сохранены как
независимые overrides поверх пресета; локальная среда отдельно управляется через `--fog-extinction=`,
`--fog-albedo=`, `--fog-anisotropy=` и `--fog-range=`. Ими изолируется один consumer для A/B.
Профиль отдельно задаётся `--fog-base=` и `--fog-height=` (экспоненциальный scale height), пространственное
поле — `--fog-variation=`, `--fog-cell=` и `--fog-speed=`. Четыре
астрономических пресета PF07 сохранены буквально. `compare_pf07_baseline.sh` запускает ТОЛЬКО PF08 и
сравнивает его с четырьмя уже зафиксированными PNG PF07; сам PF07 повторно запускается только при явном
обновлении его `reference_frames/`.

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
   существует, поле в state не добавляется «на будущее»: `overcast`, `rain`, `snow` появятся только вместе
   с froxel-облаками и осадками.
2. **DONE — froxel medium.** Проверяемый однородный интеграл как первый proof, затем общий экспоненциальный
   высотный профиль и advected world-space density field для объёма и поверхностей; оба светила, луны,
   атмосферное прохождение и shadow visibility.
3. **DONE — finite cloud layer.** `cloudy|overcast`, 3D world-space density, общий ветер, два светила,
   self-shadowing и совпадающая с объёмом движущаяся тень на поверхности.
4. **Precipitation across distance.** Near drops/flakes, mid/far extinction, укрытие и impact-события.
   PF05 даёт проверенный particle lifecycle, но его camera-local billboard rain не считается готовой погодой.
5. **Wet world and screen manifestations.** Накопление/высыхание мокроты, roughness/specular response,
   лужи и рябь; lens droplets только как следствие попадания воды, с отдельным A/B.
6. **Закрывающий аудит.** Фиксированные clear/overcast/rain/snow кадры в нескольких временах суток,
   временные переходы, GPU budget, Vulkan validation и проверка того, что clear всё ещё совпадает с baseline.

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
