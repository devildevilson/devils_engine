# PF10 — functional planet

PF10 проверяет не «похожий на планету шар», а пригодную для игры сферическую поверхность. У неё есть
реальная геометрическая высота, тысячи стабильных политических областей, подписи и объектные anchors.
Рельеф и вся семантика принадлежат планете: при повороте глобуса они не плывут в world/screen space.

**Первое отображение работает (2026-08-30).** Это один близкий глобус без атмосферы и прочего окружения.

## Запуск

```bash
cmake --build build-debug --target PF10_functional_planet -j2
./build-debug/subprojects/playgrounds/PF10_functional_planet/bin/PF10_functional_planet
./build-debug/subprojects/playgrounds/PF10_functional_planet/bin/PF10_functional_planet --verify
./build-debug/subprojects/playgrounds/PF10_functional_planet/bin/PF10_functional_planet \
  --fixed-rotation --frames=8 --shot=/tmp/pf10.ppm
./build-release/subprojects/playgrounds/PF10_functional_planet/bin/PF10_functional_planet \
  --distance=1.20 --frames=80 --shot=/tmp/pf10_provinces.ppm
```

Управление:

- `WASD` — вращать планету;
- колесо — приблизить/отдалить единственный глобус;
- `LMB` — закрепить область под курсором;
- `R` — остановить/возобновить медленное вращение;
- `B` — переключить один из четырёх цветов всех границ;
- `P` — political/terrain A/B; вместе с political выключаются временные labels;
- `O` — показать/скрыть object anchors;
- `Esc` — выход.

## Что уже нарисовано

### Настоящая сферическая высота

Канонический адрес поверхности — единичное planet-local направление `d`; точка имеет вид
`p=d*(R+h(d))`. Transform применяется после вычисления высоты. Это vertex displacement: меняются позиции,
depth, производные normals и силуэт. Значение ограничено `[-0.045R,+0.085R]`; текущая детерминированная
выборка заняла `[-0.03659R,+0.05864R]`. Амплитуда намеренно крупная, потому что первый fixture доказывает
геометрию, а не физический масштаб конкретной планеты.

Глобус — cube-sphere: default `6 x 256 x 256` клеток и `786 432` треугольника. Высота один раз запекается
в `6.34 MB` planet-local position buffer; кадр больше не вычисляет четыре октавы шума на каждой вершине.
Каждая строка грани рисуется отдельным instance triangle strip, поэтому остаётся около `790k`, а не
`2.36m` повторных vertex invocations. Fragment normal выводится из производных уже смещённой world position.
`--mesh=32..256` меняет геометрическое разрешение.

### Области планеты

Форма первого fixture намеренно временная. Land-провинции получаются локальным 3D Voronoi вокруг сферы:
каждый канонический запрос рассматривает только `3x3x3=27` соседних cells, а не перебирает все области.
Редкий survey из `600 000` направлений видит `4012` land ID, а плотный `6 x 513 x 513` bake материализует
**4072 игровые провинции** — маленькие области больше не теряются. Это сразу держит целевой порядок
`3–5 тысяч`, а не демонстрационный десяток.

Два верхних бита ID резервируют классы области:

- land province — минимальная будущая единица навигации и соседства;
- четыре большие water region — ограниченные морские узлы, где позже возможен переход port-to-port;
- две non-playable polar region — север и юг.

Внутри land-провинции PF10 не создаёт скрытых клеток навигации. Персонаж будет другой минимальной единицей,
но персонажей и собственно pathfinding в этой площадке пока нет. Граф уже материализован как CSR:
`4072` узла, `12 895` undirected edges, средняя степень `6.33`, одна land-компонента и ни одного изолированного
узла. Ребро появляется только когда два ID встречаются по соседним samples общей сферической границы;
близость центров и цвет пикселя источником связности не являются. Узел дополнительно хранит стабильный ID,
приближённое centre direction и coastal flag. Water port-to-port graph остаётся отдельным будущим контрактом.

Граница вычисляется как расстояние до равенства первого и второго Voronoi-кандидата; coast и polar boundary
дают такое же непрерывное расстояние. Канонический evaluator запускается один раз при старте и кладёт
`region_id + edge_distance` в `12.63 MB` статический cube atlas. В кадре остаются cached reads вместо
27 Voronoi-кандидатов на пиксель. `fwidth` сохраняет заметную линию при зуме, `B` доказывает, что цвет
границы — данные кадра, а не зашитая часть province colour.

### Interaction, text и объекты

CPU picking переводит camera ray обратно в planet-local space, находит первое пересечение с
`R+h(d)` marching+bisection и запрашивает тот же region evaluator. Hover и selection сравнивают стабильный
ID в shader; скрытую область за глобусом выбрать нельзя.

Подписи используют **тот же Crimson atlas, glyph metrics, depth reconstruction и oriented projection volume**,
что screen-space decals PF05. Это не screen-aligned текст: fragment восстанавливает точку настоящего рельефа
из `scene_depth`, переводит её в planet-local glyph volume и только там читает MSDF. Поэтому надпись
вращается с планетой и повторяет поверхность. Owner-ID не даёт provincial decal вытечь в соседа или воду.

LOD сейчас намеренно простой: дальше `1.72R` рисуются три временных имени крупных областей (`31` glyph),
ближе — placeholder `P0001...` для каждой видимой провинции (`20 360` glyph records на всю планету;
обратная сторона отсекается до rasterization). Имена и имперская группировка — будущий content, механизм
масштабирования уже виден. `--distance=1.16..4.5` позволяет фиксировать оба режима.

Двадцать четыре planet-local object anchors показывают три будущих класса: city, wonder и construction.
Маркер хранит `region_id`, направление, высоту, тип и масштаб; `--verify` повторно запрашивает поверхность и
убеждается, что anchor всё ещё принадлежит записанной land-провинции. Никакой логики города/стройки здесь нет.

## Производительность и проверки

Первый честный uncapped Release замер выполнял procedural height в vertex и 27-cell Voronoi во fragment:
`15.906 ms`, или `62.9 FPS` GPU-equivalent на Iris Xe при 1280x720. После статического bake, triangle strips
и политического atlas итог остаётся около `2.2–2.5 ms` (`~390–450 FPS`) в зависимости от масштаба. Ближний
кадр с полным province-label LOD: scene около `2.10 ms`, decals `0.31 ms`, весь кадр `2.54 ms` (`393 FPS`).
Это выше требуемых `200–250 FPS` даже с запасом; pacing выключен, GPU timestamps измеряют passes отдельно.
Статические surface/politics/label данные занимают примерно `22.6 MB`; в каждом кадре они не пересоздаются.

`--verify` сейчас даёт `18/18`:

- land count лежит в `[3000,5000]` (`4012`);
- water regions крупные и немногочисленные (`4`), polar regions ровно две;
- высота ограничена и имеет наблюдаемый радиальный диапазон;
- повторный survey даёт fingerprint `0x274ca62212fa247e`;
- плотный bake покрывает survey, CSR полон, симметричен, не имеет self/isolated nodes и держит mean degree
  в `[4,8]` (`6.33`);
- label anchor каждой провинции является положительно удалённой от границы внутренней точкой;
- высота и `province_id` сохраняются после planet transform;
- 24 object anchors принадлежат записанным провинциям;
- центральный camera ray выбирает видимую displaced surface, а луч мимо планеты ничего не выбирает.

Debug и Release targets собираются. Fixed far/near 1280x720 frames проходят; Vulkan validation на пути
planet + markers + depth-reconstructed MSDF decals + overlay не сообщает VUID/API warning/error.

## Следующие срезы

1. **Water navigation.** Определить отдельный port-to-port graph и запрет навигации через polar regions;
   land CSR уже материализован.
2. **Canonical data.** Отвязать временную форму областей от процедурного fixture: GPU-представление
   становится проверяемым bake канонических province records, а не второй истиной.
3. **Editing and persistence.** Изменение высоты/принадлежности обновляет render, picking, border и graph;
   save/load возвращает тот же fingerprint и selection.
4. **LOD and residency.** Приближение от глобуса к участку, crack-free стыки и стабильные
   height/normal/border/ID/text/object anchors.
5. **Audit.** Детерминированные globe/terrain/political/selected frames, budgets и явные production limits.

## Не входит сейчас

- генерация провинций из геологии, климата либо истории — текущая форма произвольна сознательно;
- персонажи, армии, города как gameplay, экономика и правила владения;
- атмосфера, облака, день/ночь и механика PF07/PF08;
- отдельная отражающая ocean surface, plate tectonics и erosion;
- бесшовный спуск от орбиты до сантиметров;
- source/CMake dependency на PF05 или PF09. PF10 самостоятельно повторяет только доказанные контракты,
  необходимые этому fixture.
