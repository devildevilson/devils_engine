# PF08 — weather effects

Статус: **нулевой срез CLOSED — PF07 baseline перенесён и зафиксирован** (2026-08-28).

PF08 проверяет погоду как состояние открытого мира, а не как отдельный дождевой emitter. Площадка начинает
с независимой копии закрытого `PF07_party_environment`: та же P-type двойная система, календарь и затмения,
физическая атмосфера и экспозиция, две системы каскадных теней, proxy-долина и `6000` кустов, читающих одно
ветровое поле. PF07 остаётся неизменяемой контрольной стороной; вся дальнейшая погодная работа принадлежит
PF08 и не создаёт CMake/source dependency между лабораториями.

## Запуск

```sh
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects
build-release/subprojects/playgrounds/PF08_weather_effects/bin/PF08_weather_effects --verify
./subprojects/playgrounds/PF08_weather_effects/compare_pf07_baseline.sh
```

Все CLI-рычаги и четыре астрономических пресета PF07 пока сохранены буквально. Это намеренно: до появления
первого погодного consumer clear-state обязан пройти `41/41` численную проверку и дать те же пиксели в
`noon`, `double_sunset`, `night`, `eclipse`. Скрипт сравнивает сырые PPM, поэтому метаданные PNG и timestamps
не могут спрятать расхождение.

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
1. **Weather state.** Данные и пресеты `clear|overcast|rain|snow`, переходы без перестройки render graph,
   общее ветровое поле и диагностическая визуализация значений. Пока consumer не существует, поле в state
   не добавляется «на будущее».
2. **Froxel medium.** Сначала однородный туман как проверяемый интеграл, затем высотная/адвектируемая
   плотность и облачный слой; оба светила и shadow visibility используют один интеграл.
3. **Precipitation across distance.** Near drops/flakes, mid/far extinction, укрытие и impact-события.
   PF05 даёт проверенный particle lifecycle, но его camera-local billboard rain не считается готовой погодой.
4. **Wet world and screen manifestations.** Накопление/высыхание мокроты, roughness/specular response,
   лужи и рябь; lens droplets только как следствие попадания воды, с отдельным A/B.
5. **Закрывающий аудит.** Фиксированные clear/overcast/rain/snow кадры в нескольких временах суток,
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
