# Аудит шейдеров PF03

Дата закрытия: 2026-08-21. Проверены все 31 файла в `resources/shaders`: 27 самостоятельных стадий и четыре
общих include-файла. Цель аудита — проверить не только компиляцию, но и смысловые контракты между разрешениями,
историей, alpha-каналами, mip-уровнями и render config.

## Итог

Блокирующих дефектов после исправлений не осталось. Все 27 entry shader компилируются `glslc` и проходят
`spirv-val`; post-граф с дробным render scale, TAAU, Hi-Z SSR, DoF и motion blur, а также forward/MSAA граф
проходят Vulkan validation без ошибок. Каждый shader теперь содержит в начале человеческое описание алгоритма
и причины выбранного устройства.

## Найдено и исправлено

| область | дефект | исправление / проверяемый контракт |
|---|---|---|
| G-buffer motion | low-resolution `gl_FragCoord` делился на display extent, поэтому current и previous UV жили в разных пространствах | render extent передаётся явно в `blur_params.yz`; на масштабе 0.5 UV снова покрывает весь `0..1` |
| TAAU | Catmull-Rom sample ошибочно считался полным temporal-наблюдением для каждого display pixel, а последующий бинарный coverage заметно переключал кромки при движении | source sample вносится непрерывным tent-footprint; Catmull-Rom выравнивает spatial estimate между центрами samples |
| TAAU weight | фиксированный вес не выражал число реально собранных отсчётов и легко уходил к неотзывчивым 0.99 | отдельный full-resolution `taa_meta.r` хранит накопленный эффективный вес; history blend использует `n/(n+w)` до предела обычного `taa_weight` |
| alpha history | `taa_color.a` одновременно трактовался как rejection и fog transmittance | transmittance остаётся в alpha цвета, rejection/count вынесены в `taa_meta` |
| выключение TAA | `--taa=0` после coverage-логики перестал быть чистым upscale; при нулевом весе AO всё ещё вращал stochastic pattern | оба режима обходят coverage, AO не меняет фазу без accumulation; их итоговые кадры побитово совпадают |
| motion tiles | целочисленный `source_size / tiles` терял правый и нижний хвост на дробном масштабе | границы каждой tile вычисляются двумя рациональными долями и покрывают каждый source texel ровно один раз |
| bloom downsample | 13 весов суммировались в `1.125`, а внутренние/внешние taps не соответствовали фильтру | нормированная 13-tap схема сохраняет константный сигнал на каждом mip |
| DoF gather | комментарий обещал coarse neighbourhood CoC, но implicit `texture()` читал LOD 0 | neighbourhood читается явным `textureLod` с самого грубого доступного уровня |
| Hi-Z SSR | у строго горизонтального/вертикального луча расчёт границы мог получить `0/0` | нулевая компонента delta явно получает бесконечную дистанцию до границы |
| descriptors | три set были мёртвыми, а AO sets содержали посторонние bloom/shafts/histogram bindings | мёртвые sets удалены, layouts сужены до реально объявленных shader bindings |
| документация | histogram comments описывали старый dispatch и вводили в заблуждение | комментарии приведены к текущей схеме: один sample на thread, локальная гистограмма, не более одного global atomic на непустой bin |

## Численная проверка TAAU

Статичная камера, 32 кадра, фиксированная экспозиция, без bloom/shafts, `render-scale=0.5`; эталон — native TAA:

| вариант | RMSE, уровней из 255 |
|---|---:|
| простой Catmull-Rom upscale | 7.40 |
| TAAU с намеренно неверным знаком coverage | 6.87 |
| TAAU с бинарным coverage и count | 3.76 |
| TAAU с непрерывным tent coverage и weighted count | **2.98** |

Это закрывает прежний отрицательный результат среза 13: инфраструктура была рабочей, но неверный motion field
не позволял оценить реконструкцию. На малом повороте камеры (`orbit=0.2`, кадры 48/49) tent coverage уменьшает
межкадровый RMSE `6.65 → 5.22`, а среднюю ошибку к native TAA тех же кадров `5.29 → 4.02`; плавность получена
не простым отставанием. Дополнительная инварианта passthrough: `--taa=0` и `--taa-weight=0` дают `AE = 0`,
если прочие параметры одинаковы.

## Просмотренные группы

- Geometry/shading: `gbuffer.*`, `shade`, `pf03_shading`, `fog`, SSAO и blur — пространства координат,
  reverse-Z, роль AO только в ambient, depth-aware half-resolution contract.
- Temporal/screen-space: `taa`, Hi-Z, SSR, motion tile/dilate/blur — history ownership, mip addressing,
  disocclusion, края экрана и дробные размеры.
- Camera/optics: DoF CoC/down/gather, bloom down/up, shafts — единицы CoC, разделение планов, нормировка
  фильтров и линейный HDR порядок.
- Output: histogram clear/build/resolve, exposure, grade/LUT, compose — межшаговые usages, exposure-relative
  threshold, единственный display transfer и debug bindings.
- Reference branch: `forward.*` и `forward_output` — общая модель освещения, MSAA resolve и независимость
  от post-графа.

## Сознательно оставленные границы

- TAA/TAAU по-прежнему не имеют responsive masks для прозрачности и частиц, правил cut/телепорта и отдельного
  post-accumulation sharpen. Это production-полировка, не незаконченный базовый алгоритм площадки.
- Численный плюс TAAU закреплён на статичной сцене; движущиеся объекты и disocclusion прошли runtime/validation,
  но отдельный motion ground-truth для upscale не строился.
- Hi-Z SSR оставлен как измеренный A/B, хотя на этой сцене линейный march дешевле; это ограничение сцены и
  screen-space метода, а не причина продолжать PF03.
- Выключенные через UBO эффекты всё ещё могут исполнять pass. Устранение этой цены требует вариантов формы
  графа и относится к проекту [render profiles](RENDER_PROFILES.md).

С этими оговорками shader-часть PF03 можно считать закрытой и переходить к следующей площадке.
