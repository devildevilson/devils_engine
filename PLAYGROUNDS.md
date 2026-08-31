# devils_engine — каталог маленьких playground-проектов

Этот документ описывает ограниченные проверочные проекты. Playground — основная единица текущего
планирования: законченный срез, который можно запустить и оценить глазами/на слух либо который доказывает
крупную симуляционную систему. Нумерованные задачи движка остаются каталогом зависимостей и не задают
порядок работы сами по себе.

Playground отвечает на конкретный технический вопрос и заканчивается наблюдаемым результатом. Если он
оказался полезен, его можно расширять следующими именованными slices; некоторые стенды естественно
превратятся в ранние срезы игр. До этого момента они не обязаны иметь сюжет, баланс, production UI или
законченный art style.

## Общие правила

### Что считается хорошим playground

- Запускается отдельным executable или отдельным явно выбранным scenario.
- Имеет один главный технический риск и не более двух вторичных.
- Использует обычные engine resources, manifests и lifecycle, а не специальный тестовый путь.
- Имеет маленький authored либо generated fixture, который легко понять глазами.
- Позволяет менять ключевые параметры в runtime и видеть промежуточные состояния.
- Поддерживает headless validation там, где результат не обязан оцениваться глазами.
- Имеет измеримые критерии завершения: frame time, determinism, memory, latency, artifact hash или
  корректный state transition.
- Имеет свой `README.md`: назначение, запуск, точки входа, debug views, граница и Definition of Done.
- Может быть удалён без потери проектного контента: ценность остаётся в закреплённом engine contract,
  tests и reusable resources/tools.

### Чего в playground быть не должно

- попытки сразу собрать полный gameplay loop;
- production-контента и большого числа уникальных assets;
- временной архитектуры, обходящей обычный `simul`/`demiurg`/`painter` path;
- project semantics внутри engine только ради удобства одного стенда;
- нескольких крупных неизвестных одновременно;
- требования «выглядеть как готовая игра».

### Общая оболочка

Полезно дать всем graphical playgrounds одну небольшую оболочку:

- свободная и фиксированная camera;
- pause, single-step и time scale;
- выбор scenario без перекомпиляции;
- runtime parameter inspector;
- небольшой Visage overlay с названием/описанием сцены, controls и FPS/frame time;
- render-target/debug overlay;
- CPU/GPU timings и resource residency;
- capture screenshot + config/resource/build fingerprints;
- reset scenario в исходное состояние;
- deterministic seed;
- опциональный headless запуск с фиксированным числом ticks/frames.

Frame pacing этой оболочки не выводится из present mode: MAILBOX/FIFO/IMMEDIATE выбирают tearing и
поведение presentation queue, а отдельный deadline + sleep limiter задаёт частоту producer loop. На обычном
desktop предпочтителен MAILBOX-first; fallback между FIFO и IMMEDIATE выбирается явно по политике платформы.

Первый рабочий common slice уже используется PF01: `frame_pacer` держит абсолютный deadline и отбрасывает
пропущенное расписание без catch-up burst, а non-interactive Visage-overlay загружает общий Lua entry/MSDF
font и показывает описание сцены, controls, сглаженные FPS/frame time. GPU upload/font descriptor и
`draw_ui` остаются обычными ресурсами render graph конкретной лаборатории.

Это не полноценный editor. Оболочка только делает маленькие experiments наблюдаемыми и повторяемыми.

### Режим работы

- В каждый момент активна одна campaign и один её лабораторный срез.
- Мелкая задача из `ROADMAP.md` берётся только как обнаруженный blocker текущего результата.
- Числовой/focused test закрепляет контракт, но не выбирает направление работы.
- Независимая идея, не мешающая текущему срезу, остаётся в backlog/parking lot.
- Лаборатория закрывается запускаемым сценарием, debug-представлением, минимальным regression contract
  и обновлённой документацией.
- Все площадки физически живут под `subprojects/playgrounds/<CODE>_<human_name>/`; уже доказанная общая
  оболочка постепенно собирается в `subprojects/playgrounds/common/`.

## Текущий фокус — Painter visual stack

Campaign содержит десять независимых painter-лабораторий `PF01`–`PF10`: `PF01`–`PF08` закрыты,
[`PF09_territory_zoning`](subprojects/playgrounds/PF09_territory_zoning/README.md) остаётся отдельной открытой
площадкой, а текущий новый фокус —
[`PF10_functional_planet`](subprojects/playgrounds/PF10_functional_planet/README.md): displaced сферическая
поверхность, устойчивые при вращении провинции и interaction. `anomalous_weather` перенесена в ненумерованный
parking lot и не занимает следующий код заранее.
PF04 закрыт основными срезами: `D24S8`
selection/outline/debug; независимые bits `0x02/0x04` для local tint и spatial window; runtime
`StencilReference/CompareMask/WriteMask`; asymmetric front/back fixture; а после закрытия — отдельная coverage+depth selection mask,
трёхпиксельный depth-aware screen-space outline, optional through-wall selector и `depth_fail_op`-силуэт скрытой цели на bit `0x40` с явным порядком относительно portal. Внутреннее
представление dynamic states свёрнуто из растущего набора bool-полей в один массив Vulkan values;
их человеческие имена и mapping задаёт один X-macro list в `painter/common.h`.

`PF03` закрыт 2026-08-21 полной запускаемой post-цепочкой, numeric/debug контрактами и shader-аудитом.
Финальная незакрытая техника, TAAU, теперь действительно реконструирует: при масштабе 0.5 ошибка против
native TAA составляет `2.98/255`, у простого upscale — `7.40/255`; непрерывный coverage дополнительно убрал
заметное переключение кромок при движении камеры. Аудит, исправленные контракты и
сознательно оставленные production-границы записаны в
[`SHADER_AUDIT.md`](subprojects/playgrounds/PF03_post_processing/SHADER_AUDIT.md). Проект build-вариантов уже
выбранного render graph, declared-value references и частичных material/step patches вынесен в
[`RENDER_PROFILES.md`](subprojects/playgrounds/PF03_post_processing/RENDER_PROFILES.md): основной state —
независимые graph-specific settings, общий preset лишь массово их записывает, а concrete variant выводится
resolver'ом. Реализация остаётся отдельным backlog, а не блокером следующей площадки.

Лаборатории не образуют CMake/source dependency chain. Более поздняя площадка может выборочно взять
зафиксированный baseline ранней либо общий код из `common`/`libs/painter`, после чего развивается
независимо. Поэтому расширение ранней gallery не меняет автоматически `PF06`.

## Painter campaign: десять лабораторий

| Порядок | Директория | Наблюдаемый результат | Первый consumer |
| --- | --- | --- | --- |
| 1 | `PF01_forward_plus` | много движущихся lights, cluster heatmap и Forward+/simple comparison | общий renderer |
| 2 | `PF02_shadows` | directional/spot shadow maps, atlas и bias diagnostics | общий renderer |
| 3 | `PF03_post_processing` | независимая расширяемая post-effect gallery | общий renderer |
| 4 | `PF04_stencil_effects` | outline, local mask и portal/mirror/window proof | общий renderer |
| 5 | `PF05_scene_effects` | 3D SDF, decals, particles/weather, cel shading, billboards и world-space UI | общий renderer |
| 6 | `PF06_submarine_light_room` | густая тёмная сцена со светом как препятствием | `submarine_coop` |
| 7 | `PF07_party_environment` | небесная механика, динамический свет и окружение | `party_adventure` |
| 8 | `PF08_weather_effects` | погода в сцене PF07: состояние, объём и экранные проявления | `party_adventure` |
| 9 | `PF09_territory_zoning` | стриминг территорий, места из выпуклых частей, граф проходов в файле, двери-зоны, этажи, предметы, тактика, титулы и право на землю | `medieval_hero_manager` |
| 10 | `PF10_functional_planet` *(active)* | настоящий сферический рельеф, политические области и выбор | planetary strategy / world view |

## Кампания генератора

Вторая активная кампания. Отвечает на вопрос не «как выглядит мир», а «как устроен генератор»:
типизированные буферы, шаги из конфига, lua как программируемая команда шага, воспроизводимость.
Реализуется библиотекой [`libs/originator`](libs/originator/README.md) и закрывает задачи
`GEN-01`…`GEN-13` из `ROADMAP.md`.

| Порядок | Директория | Наблюдаемый результат | Первый consumer |
| --- | --- | --- | --- |
| 1 | `GN01_generator_contract` *(active)* | буферы/шаги/lua-тела, апертуры, три уровня исполнения, области | все проекты с генератором |

[`GN01_generator_contract`](subprojects/playgrounds/GN01_generator_contract/README.md) закрыл первый вертикальный срез
2026-08-31: пайплайн описан в `tavl`, тела шагов — lua, параллельное исполнение совпадает с
последовательным побайтово при 1/3/7 потоках, а граница уровней измерена на `4 194 304` элементах —
нативное ядро `3.35` нс/элемент, `devils_script` `51.79`, поэлементный lua `106.18`, причём у
последнего параллельного пути нет вовсе. Отсюда правило: lua обходит только те множества, которые
сама перечислила. Подключены FastNoise2 (дерево узлов = закодированная строка в конфиге) и jc_voronoi
(соседство областей из Делоне), а оба инструмента `scatter` (`group_by`, `accumulate`) дают побитово
одинаковый результат при любом числе потоков и параллелятся почти линейно.

`PF01`–`PF08` закрыты. PF07 зафиксировал P-type систему, календарь, атмосферу, экспозицию/colour script,
тени двух светил, proxy-долину и инстансные заросли; PF08 независимо наложила на эту сцену единый weather-state,
высотный туман, конечные 3D-облака, near/mid/far дождь и снег, видимое укрытие, дешёвую world-space память,
радугу и снежные блики, универсальную молнию и planet-anchored aurora shell. Её headless контракт — `117/117`.
Восемь постоянных closing-audit кадров покрывают clear в трёх временах суток, overcast, rain, snow, lightning
и aurora; повторный PF08-only прогон совпадает с ними побитно. Пять активных Vulkan-validation путей чисты,
а худший зафиксированный Iris Xe кадр при 1280x720 занимает `10.038 ms`. PF07 при этом не переснимается:
старый baseline использует его сохранённые PNG, а новая PF08-галерея имеет собственные frozen frames.
Обе project-look сцены фиксируют только нужное им подмножество
gallery-возможностей в собственных resources/presets. Исходники и CMake targets лабораторий
не зависят друг от друга.

Параллельно открыта [`PF09_territory_zoning`](subprojects/playgrounds/PF09_territory_zoning/README.md) —
каркас территорий для `medieval_hero_manager`. Площадка начиналась с единого иерархического разбиения
плоскости и растра по нему; эта постановка ЗАКРЫТА и перестроена, потому что игре нужно оказалось другое.
Несущая конструкция теперь такая: **зона — это МЕСТО из выпуклых частей, а проход — ЗАПИСЬ В ФАЙЛЕ**;
всё это лежит в секторных файлах и подгружается с диска по мере движения игрока. Общее ребро — лишь один
из способов эту запись получить, и работает он ровно один раз, в сборщике: совпадение рёбер ненадёжно как
источник связности, а вычисляемая на лету связность начинает мигать на округлении. Что связность обязана
быть данными, доказывают два случая, и оба есть в фикстуре: связь БЕЗ ребра (лестница на второй этаж,
дорога между поселениями) и ребро БЕЗ связи (верхний зал лежит в плане ровно над нижним, «общее ребро»
идеальное, а между ними перекрытие). Отрезок в портале хранится затем, чтобы по нему можно было
прицелиться, как в navmesh.

Дверь при этом — не особый вид связи, а МЕСТО с переключаемой проходимостью: у двери четыре ребра, и
запертость на ребре пришлось бы держать согласованной на всех четырёх, а у места она одна. Состояние
переживает выгрузку сектора, и маршрут меняется со следующего же поиска. Здание — не фигура, а абстрактный
узел-группа, и вопрос «здание и комната накладываются» исчез вместе с его формой.

Отдельный вывод про координаты: зонируется НЕ планета, а окрестность партии — дальние города симулируются
фоном и зон не имеют вовсе. Область сборки переехала к началу координат, где шаг `float` равен четырём
миллиметрам вместо трёх сантиметров, и целый класс ошибок точности, стоивший площадке трёх разборов
подряд, исчез вместе с источником: щели округления задевают ноль точек выборки из `4096` против одной.

Три правила модели: зоны НЕ замощают пространство (пропуск между ними — законный ответ, а не дефект);
уровни НЕ образуют дерева (у каждого своя карта взаимодействий, ссылка вверх справочная); связность — это
и есть содержание.

Внутри мест появились ПРЕДМЕТЫ — стулья, столы, бочки, камни, — и они ломают то, на чём держалась вся
модель движения: «часть выпукла, значит внутри неё можно идти по прямой к проёму». Ответ — НЕ делать
предмет зоной: зонами предметы превратили бы комнату в лоскутное одеяло, а связность — в граф, который
надо пересобирать всякий раз, когда подвинули стул. Часть остаётся единицей связности, маршрут о мебели
не знает вовсе, а обход предмета — местная задача шага. Что это работает, проверяется не отсутствием
ошибок, а тем, что те же `64` маршрута всё ещё проходятся целиком при `17 760` предметах на пути.

На предметах стоят ТАКТИЧЕСКИЕ ЗАПРОСЫ, ради которых зонирование и затевалось: где спрятаться, откуда
виден вход, куда развести группу. Все три — на одном примитиве, видимости, и границей видимости служит
само место: вышел за его пределы — значит между точками стена, потому что покрытие полное и снаружи стоит
соседнее место. Отдельный список стен не понадобился, и вот это прямой выигрыш от полного замощения.
Укрытие ПРОВЕРЯЕТСЯ, а не выводится, и его авторство честное: если подход к предмету со стороны угрозы
тоже не просматривается, прячет форма места, а не предмет, — иначе игра сожгла бы ящик и удивилась, что
укрытие осталось. Приказ игрока «держим этот дом» разворачивается в разные задачи каждому: `fan_out`.

Над всем этим появился КОНТРОЛЬ, и его главное решение: **ответ несёт ровно один уровень, и не тот, на
котором спрашивают.** У комнаты нет своего уровня преступности — он есть у района, и комната его наследует;
у района нет своей торговли — она есть у поселения. Запрос это подъём по вложенности до носителя, а не поле
в каждой записи: держать преступность у каждой комнаты значило бы завести десять тысяч чисел, обязанных
меняться вместе. Отсюда прямо следует то, ради чего иерархия и заводилась: **уровни ВПРАВЕ расходиться** —
район держит одна сила, город другая, и в этом расхождении живёт игра (`13` районов из `44` в фикстуре).
Навигация это учитывает ценой, а не запретом: запрет означал бы «пути нет», когда единственная дорога идёт
через чужой квартал, — а идти туда бывает надо, просто дорого.

Над контролем встало ПРАВО, устроенное как система титулов CK. Титулы — отдельная от зон структура, и
разделение несущее: у зоны есть пространственная вложенность («где это»), у земли — юридическая
принадлежность («чьё это»), и совпадать они не обязаны — дом стоит на улице, но принадлежит не улице.
Обойтись одной иерархией значило бы сделать «переехал» и «сменил хозяина» одним событием. **Титулы не
стримятся**: земля подгружается, а «кто правит этим городом» обязано отвечаться всегда, иначе право
собственности зависело бы от того, где стоит партия.

У титула есть де-юре родитель (карта права) и де-факто держатель (карта силы). Держатель признаёт власть
уровня выше, если законен сам; узурпатор — нет, и на нём цепочка признаний обрывается: в законопослушном
квартале действует закон державы, в занятом — его собственный, а **занятый дом посреди законопослушного
квартала оказывается местом без закона вовсе** (собственность законов не издаёт). Частную территорию
открывает ДЕ-ФАКТО держатель, а не законный владелец: дом, занятый революционером, закрыт и для хозяина,
причём право хозяина никуда не девается. Навигация уважает это ценой, а не запретом.

Улица заодно нарезана на полосы: мостовая посередине, обочины по краям, разница выражена СКОРОСТЬЮ, а не
флагом «дорога». Полосы кладутся параллельно улице — иначе дверь дома выходила бы на проезжую часть.

Отрисовка доведена до партийной РПГ: наклонная камера, этажи с переключением (`Z`/`X`), срез переднего
плана расширяющимся к камере клином (`C`), маршруты по серединам проёмов из ТОГО ЖЕ пути, по которому
персонаж шагает (`V`), и мировой интерфейс над персонажами — якорь в мире, раскладка в пикселях (`N`).

Померено: `6x6` секторов по `8192 м` дают `12 009` зон, `155 248` проходов и `8.0` МБ на диске, то есть
весь мир при той же плотности — `3.4` ГБ и `5` млн зон, и это оценка сверху. `696` дверей (`93` заперты в
файле), `117` лестниц, `234` зоны выше первого этажа, `466` рёбер графа без геометрии. Переключение двери
изменило маршрут в `48` случаях из `48`, и ни один маршрут не прошёл сквозь закрытое место. `17 760`
предметов; из `2 609` найденных укрытий `2 381` даёт предмет и `228` — форма места. При весе преступности
`6` средняя преступность маршрута падает с `482` до `394`: тише у `38` пар из `96`, хуже ни у одной.
`1 591` титул на область; `2 055` мест под законом державы и `859` под законом своего квартала. Мостовая
берёт `55%` шагов при трети площади улицы. Законопослушный проходит `19` чужих шагов против `27` у
безразличного. Движение: `64` маршрута между комнатами, `46 000` шагов, персонаж всё время внутри своей
зоны и ни разу внутри предмета. `140` численных проверок на девяти сидах.

Дефекты, которые нашлись только проверками: остов для замков строился по всем рёбрам, включая стены (стена
давала мнимую связность, и настоящая дверь запиралась как лишняя, отрезая кусок города); рёбра выводились
между зонами РАЗНЫХ уровней (персонаж мог «войти в поселение» как в комнату); рёбра графа у зоны без формы
молча терялись, и весь политический уровень уезжал на диск набором изолированных точек; здание разъезжалось
по секторам, потому что сектор выводился из центра габарита, а у зала, лестницы и стен центры разные. И
отдельный урок про сами проверки, полученный трижды: **проверка, которая не может провалиться, хуже
отсутствующей** — угроза в проверке укрытий стояла СНАРУЖИ места и фильтр «не просматривается» пропускал
всё; расхождение уровней контроля насчиталось у всех `44` районов, потому что у одного из уровней потерялся
носитель и он отвечал нулём; а когда образец не находился, шесть утверждений молча не выполнялись, и
единственным следом было «123 проверки вместо 129». Отдельно записан и класс ошибок времени жизни:
`zone_record*` смотрит внутрь резидентного сектора и после `focus` становится висячим — через границу
подгрузки носят КЛЮЧИ, а не указатели. От
прежней постановки в ядро переехала разрежённая локальность; клипмап идентичности из несущей конструкции
выпал.

**Ревью на закрытии** записано в README площадки и закончилось уборкой. Несущими оказались четыре
решения, но переезжать в движок должно только то, что шаблонизируется без выдумывания: **правило и
библиотека — разные вещи.** Переехало одно — «ответ несёт ровно один уровень» стало
`utils::nearest_carrier`; плюс то, чего в списке не было: **таблица рантайм-подмен была написана в
площадке ЧЕТЫРЕ раза** (двери, контроль, занятые места, титулы) и стала `utils::override_table`.
«Связность — данные» и «избегание — цена» остались правилами: обобщать там нечего, кроме графа и функтора
стоимости. Дерево титулов с де-юре/де-факто — игровая механика, кандидат в отдельную библиотеку, а не в
`utils`.

Выкинуто: клипмап (`1 196` строк, `19` проверок и `3.49 с` из `7.24 с` времени `--verify`) вместе с
`resolve_single`, существовавшим ровно для GPU-запекания; мёртвые значения формата; две тавтологичные
проверки. Итог уборки: **`9 043` строки вместо `10 239`, `--verify` за `3.5 с` вместо `7.2 с`.** Поиск
пути отдан общему `devils_engine::astar` с нулевой эвристикой — стоимость шага здесь не расстояние, и
завышенная оценка снизу сделала бы A* просто неправильным. Появился первый замер времени: `0.26` мс и
`376` развёрнутых узлов на маршрут; он же показывает границу, за которой линейный перебор открытого и
закрытого списков внутри движкового A* станет квадратичным. Остаётся долгом линейный `zone_store::sector()`
и отсутствие проверок производительности стриминга — сознательно оставлено на другой раз.

### Текущая PF10: functional planet

[`PF10_functional_planet`](subprojects/playgrounds/PF10_functional_planet/README.md) строит функциональную
планету, а не затенённый гладкий шар. Канонический адрес поверхности — единичное направление `d` в локальной
системе глобуса; вершина лежит в `p=d*(R+h(d))`, поэтому высота меняет геометрию, depth и силуэт и вращается
вместе с планетой. Bump/normal map без displacement этот контракт не закрывает.

То же направление независимо отображается в стабильный `province_id`. Политический слой получает границы,
соседство, hover и закреплённый selection, но не становится источником формы рельефа. Первый fixture — один
вращающийся глобус с нарочито читаемой высотой и несколькими десятками spherical Voronoi provinces без
longitude seam и особой логики полюсов. PF09 остаётся источником доказанных идей идентичности и interaction,
но его плоские sector polygons не становятся зависимостью или геометрией PF10.

Первое отображение уже запускается: cube-sphere `6x256x256` даёт `786 432` displaced-треугольника, а
плотный политический bake — `4072` land-провинции, четыре крупные water region и две polar region.
Материализованный CSR-граф имеет `12 895` undirected edges, mean degree `6.33`, одну land-компоненту и
никаких isolated nodes. Границы меняют цвет, camera ray выбирает displaced surface, 24 anchors показывают
city/wonder/construction. MSDF-текст теперь является depth-reconstructed planet-space decal: дальше видны
имена крупных областей, ближе — placeholder каждой провинции, с owner-ID clipping по её поверхности.
One-time surface/political bake и triangle strips снизили uncapped Iris Xe GPU frame с `15.906` до примерно
`2.2–2.5 ms` при 1280x720; полный ближний label pass стоит около `.31 ms`. `--verify` — `18/18`.
Следующий контракт — отдельный water port-to-port graph; полюса остаются non-playable.

### Parking lot: anomalous weather

Будущая `anomalous_weather` должна исследовать не ещё десять названий для дождевого preset, а общий язык
**аномальных явлений**. Она независимо берёт зафиксированную сцену и доказанные механизмы PF08, не создавая
source/CMake dependency на закрытую площадку. Главный вопрос: как одним renderer-контрактом описывать
локальное событие, погодную область, верхнеатмосферную оболочку и явление космического масштаба, сохраняя
понятный контроль изнутри и снаружи эффекта. Номер площадки будет назначен, когда эта работа вернётся в
активный фокус.

Редкая реальная сторона площадки собирает эффекты, которые PF08 сознательно оставила за своей границей:

| Семейство | Явления | Новый вопрос |
|---|---|---|
| Ледяная атмосферная оптика | `22° halo`, sundogs/parhelia, circumzenithal arc, световые столбы | ориентированное спектральное рассеяние и преломление на кристаллах; две звезды создают две пересекающиеся системы дуг |
| Обратное рассеяние и дифракция | glory, Brocken spectre, белая fogbow, corona солнца/луны, иризация облаков | observer-anchored anti-solar geometry, узкие phase peaks и спектральный размер капель |
| Незавершившиеся осадки и сильный ветер | virga, rain shafts, microburst, diamond dust, позёмка и снежные языки | высотное испарение, локальные нисходящие потоки и повторный подъём surface precipitation |
| Высокие слои атмосферы | noctilucent и nacreous clouds | разрежённая верхняя оболочка, которая рассеивает свет из-за горизонта, а не испускает его как aurora |
| Верхнеатмосферное электричество | sprites, elves и blue jets | перенос универсального lightning event из облаков на высоты десятков километров |
| Редкие формы облаков | lenticular, mammatus, shelf/roll clouds | управляемое world-space density authoring и деформация общим ветровым полем |
| Рефракционные явления | нижний/верхний мираж, Fata Morgana, heat shimmer и green flash | искривление луча градиентом показателя преломления вместо обычного Beer–Lambert march |
| Фаза осадков | град, freezing rain и прозрачная наледь | температура, столкновения и material-specific response вместо универсальной «мокрости» |

Нереальная сторона не обязана притворяться физикой. Она использует те же пространственные контракты, но
явно помечает художественное нарушение и должна позволить собирать, например:

- дождь, снег, пепел или светящиеся частицы, падающие вверх, к выбранному телу либо вдоль произвольного
  векторного поля; боковые стены дождя, спиральные осадки и фронты, остановленные в воздухе;
- локальные штормовые сферы и движущиеся погодные «существа», внутри которых другие свет, гравитация,
  направление ветра и течение времени, но граница остаётся наблюдаемой с обеих сторон;
- невозможные облака — кольца, винтовые башни, идеально плоские стены, левитирующие океаны и структуры,
  которые отбрасывают тень прежде, чем закрывают светило;
- руническое северное сияние, занавеси до самой земли, несколько магнитных полюсов, aurora вокруг лун и
  управляемые цветом/музыкой небесные поля;
- чёрное солнце, отрицательные радуги, цветные затмения, световые трещины в небе, порталы, ложные созвездия
  и геометрические «шрамы» на небесной сфере;
- магические грозы: молнии между облаками, землёй, персонажами и лунами; медленные устойчивые каналы,
  шаровые разряды, цепные дуги и гром, который приходит раньше вспышки как намеренный world rule;
- метеорные ливни, кометные хвосты, падение звёзд, планетарные кольца и их тени, stellar flares, видимые
  ударные волны и туманности, которые проходят через небо как реальная объёмная область;
- смешанные явления: дождь из метеоров внутри aurora, радуга вокруг чёрного светила, снежная буря в невесомости
  или космический фронт, превращающий обычную погоду в магическую по мере движения через мир.

Предварительные срезы после возвращения площадки:

1. **Frozen inheritance.** Выбрать минимальный PF08 baseline, собственные reference frames и нулевую стоимость
   при выключенных аномалиях; никаких изменений закрытой PF08.
2. **Ice-crystal optics.** Гало, паргелии, столбы и дуги от обоих светил с раздельными physical/art controls.
3. **Observer and refractive optics.** Glory/fogbow/corona, мираж и heat shimmer; определить, где достаточно
   angular sky evaluation, а где необходим bent-ray либо screen/depth distortion.
4. **Rare volumes.** Virga, microburst, blowing snow, high clouds и upper-atmosphere lightning как расширения
   существующих world fields и spherical shells.
5. **Anomaly grammar.** Единое описание области, системы координат, времени, vector field, emission,
   extinction и связи с celestial bodies; магический эффект собирается данными, а не новым shader на имя.
6. **Cosmic weather.** Метеоры, кометы, stellar events, рифты и другие планетарные/небесные phenomena с
   корректным масштабом, окклюзией, экспозицией и наблюдением из разных точек мира.
7. **Composition and audit.** Сочетания нескольких аномалий, переходы через их границы, GPU budgets,
   deterministic gallery и явное различие между physically based, physically inspired и impossible режимами.

Ключевая защита от превращения площадки в мешок спецэффектов: каждый срез обязан добавить либо проверить
новый пространственный/оптический механизм. Новый цвет дождя или ещё одна форма облака без нового вопроса
остаётся preset/content, а не отдельным техническим результатом.

Текущие ограниченные результаты PF05 — запускаемые Crimson MSDF и screen-space decal slices: один atlas обслуживает UI,
индивидуальные world glyph matrices на прямой/quadratic Bézier и три billboard space — spherical world-size,
cylindrical/Y-locked и constant-pixel world-anchored screen-size. Fixed font height обрезает хвост по длине,
при одной длине размер выводится из метрик, а optional detail texture стилизует fill без изменения MSDF coverage.
World/billboard glyph coverage теперь пишет depth. Настоящий decal pass растеризует ориентированные box-volume,
восстанавливает world position из opaque depth, ограничивает её через `world_to_decal`, фильтрует receiver по
scene normal и проецирует MSDF на дальнюю и боковую стены. `F`/`--no-decals` дают наблюдаемый A/B.
Третий срез добавляет persistent GPU spark partition на 2048 stable slots: compute читает `history=1`, выполняет
spawn/lifetime, semi-implicit Euler, gravity/drag и bounce от аналитических границ комнаты; procedural spherical
billboards проходят world depth и используют order-independent additive blending. `P` переводит emitter из
emitting в draining/stopped и обратно, `R` очищает pool. Fixed-step rail после stop совпадает с пустым pool
(`AE=0`) по завершении максимального lifetime.
Weather-срез расширяет общий buffer ещё на 1024 camera-local slots и переносит compute после opaque scene:
отрезок движения сравнивается с current `scene_depth`, world hit/normal восстанавливаются, sparks отражаются,
а rain/snow respawn'ятся. `T` циклически меняет быстрые rain streaks, медленные drifting snow flakes и clear.
Rain использует velocity-aligned cylindrical ribbons: мировая ось падения не зависит от camera-up, к камере
поворачивается только тонкая ширина. Явный shelter AABB закрывает semantic gap screen depth для offscreen roof;
комната стала roofed dry fixture, `H` даёт shelter A/B, а `C` — depth-collision A/B. Weather использует alpha,
sparks остаются additive, UI гарантирован отдельным последним pass.
Cel-срез добавляет runtime UBO вместо pipeline variants: `G` включает Lambert quantization, `B` меняет число
bands, а `O` выбирает screen-space outline `off/silhouette/feature`. Отдельная гладкая сфера делает уровни
наблюдаемыми; outline читает visible opaque depth/normal до decals/particles/text и поэтому не превращается в
through-wall selector. Fixed A/B различает smooth/2/5 bands и обе outline policies, повторный crop даёт `AE=0`.
World-UI срез использует отдельный native Nuklear context без Lua: bounded C++ config задаёт anchor/name/health,
до трёх status rows и image slots. Три окна проходят один `nk_convert`; command userdata становится `window_id`
в расширенной вершине, а 48-byte/window SSBO переводит локальные пиксели в world-anchor clip space. Размер следует
`reference_distance/depth` с min/max clamp и дальним fade, поэтому панель приближается вместе с объектом, не теряя
читаемость. `I` освобождает cursor, CPU hit rectangles выбирают ближайший stable id, selection слегка подсвечивает
панель и связанный scene instance. Material сохраняет reverse-Z anchor depth; hidden-behind-wall fixture совпадает
с UI-off (`AE=0`).
Лаборатории по-прежнему не зависят друг от друга; общие production fixes принадлежат `libs/painter`.

## 1. Painter visual stack

Главный вопрос campaign: можно ли последовательно доказать lighting, shadows, post-processing и stencil
на маленьких наблюдаемых сценах, а затем собрать из выбранного подмножества два разных project looks?

Общая dataflow-граница:

```text
scene instances + lights
  -> depth prepass
  -> Forward+ cluster/light assignment
  -> forward HDR rendering <- shadow maps
  -> selected post chain
  -> swapchain/debug target viewer
```

Подробная граница, точки входа и Definition of Done принадлежат README каждой директории:

- [`PF01_forward_plus`](subprojects/playgrounds/PF01_forward_plus/README.md);
- [`PF02_shadows`](subprojects/playgrounds/PF02_shadows/README.md);
- [`PF03_post_processing`](subprojects/playgrounds/PF03_post_processing/README.md);
- [`PF04_stencil_effects`](subprojects/playgrounds/PF04_stencil_effects/README.md);
- [`PF05_scene_effects`](subprojects/playgrounds/PF05_scene_effects/README.md);
- [`PF06_submarine_light_room`](subprojects/playgrounds/PF06_submarine_light_room/README.md);
- [`PF07_party_environment`](subprojects/playgrounds/PF07_party_environment/README.md).

Минимальная общая shell должна появляться из потребностей `PF01`, а не проектироваться целиком заранее.
Повторённый стабильный код camera/debug/capture переезжает в
[`common`](subprojects/playgrounds/common/README.md); renderer contracts — в `libs/painter` после
доказательства, а feature resources остаются у лабораторий.

## 4. `hierarchy_sim_lab` — симуляция иерархического AI

### Вопрос

Можно ли ограниченно, детерминированно и объяснимо выполнять решения на уровнях персонажа, группы,
организации и региона, не превращая `acumen` в политическую симуляцию?

### Представление

Стенд преимущественно headless. Его UI — не игровая карта, а несколько простых views:

- граф регионов и дорог;
- список actors и groups;
- дерево goals/operations/tasks;
- timeline обязательств и interruptions;
- ownership ресурсов;
- explanation выбранного решения;
- budgets, expanded nodes, cache hits и rejected candidates.

### Минимальная модель

- 4 региона и 5–7 дорог;
- 2 фракции;
- 3 группы по несколько logical actors;
- ограниченные ресурсы: еда, деньги и influence;
- несколько операций: travel, recruit, guard, trade, investigate;
- локальные события: нехватка еды, опасная дорога, выгодная возможность;
- разные cadences для actor/group/faction decisions.

### Последовательные slices

1. Goal candidate selection с объяснимым utility score.
2. Persistent operation instance с duration/commitment/interruption.
3. Разбиение group goal на actor tasks.
4. Reservation ограниченного ресурса или дороги.
5. Multi-cadence scheduler.
6. Aggregate actors, из которых несколько materialize в detailed state.
7. Save/resume посередине операции.
8. Serial-vs-MT и 1/2/4-worker identity.
9. Batch прогон сотен seeds со статистическим отчётом.

### Ownership

- engine: workflow cursor, scheduling, budgets, reservations primitives, traces and inspectors;
- project: politics, utility factors, operation schemas, faction relations and consequences.

### Не входит

- полноценная экономика;
- дипломатический UI;
- detailed combat;
- 3D presentation;
- тысячи типов событий.

### Definition of Done первого среза

Одинаковый seed даёт одинаковый outcome после save/resume и при разном числе workers. Для каждой
операции можно ответить: кто её начал, почему, какие ресурсы заняты, какой шаг выполняется и почему она
была продолжена, отменена или прервана.

## 5. `tower_floor_lab` — квадратный этаж и небольшой tactical loop

### Вопрос

Достаточно ли project-owned grid semantics поверх общих query/resolve/workflow primitives для
детерминированного маленького этажа?

### Минимальная карта

- квадратная grid-карта 12×12 либо 16×16;
- стены, двери, опасные клетки и один exit;
- player party из двух units;
- два типа противников;
- один интерактивный объект;
- простая отдельная map presentation.

### Игровые features

- grid movement и occupancy;
- path preview;
- line of sight и простое укрытие;
- initiative/reservation;
- melee attack и один area shape;
- один status либо environmental hazard;
- enemy scorer из двух-трёх решений;
- group exit condition.

### Generator slice

Генератор создаёт не игру, а один проверяемый floor artifact:

1. room/connection graph;
2. rasterization в grid;
3. placement start/exit/objective;
4. reachability validation;
5. простой semantic quest graph из 3–5 nodes;
6. bounded repair при недостижимом exit;
7. provenance view: какой pass создал клетку/дверь/objective.

### Не входит

- campaign;
- большой набор abilities;
- inventory/equipment;
- полноценный quest narrative;
- production procedural variety.

### Definition of Done первого среза

Один seed создаёт canonical floor bytes/hash. Этаж гарантированно проходим, actor movement и combat
возобновляются после save, а visual map и tactical state читают одну grid model без дублирования координат.

## 6. `generator_contract_lab` — typed passes и Lua glue

**Стала кампанией генератора, см. выше.** Первый срез живёт как `GN01_generator_contract`; описанные
ниже pipeline'ы остаются планом следующих срезов.

### Вопрос

Можно ли собирать разные генераторы из одного host/toolkit, сохраняя typed artifacts, determinism,
provenance, validation и bounded repair?

### Первый pipeline

Намеренно абстрактная 2D region map:

1. coarse continent mask;
2. height/noise field;
3. water and moisture propagation;
4. biome classification;
5. Voronoi-like regions;
6. adjacency graph;
7. validation и один repair pass.

### Второй pipeline

Небольшой adventure graph без production terrain:

1. hierarchy задач;
2. decomposition до конечных objectives;
3. region graph;
4. content-node graph;
5. примитивные volumes/boxes вместо финальных meshes;
6. connectivity validation.

### Что проверяется

- C++ tools регистрируются как reusable building blocks;
- отдельный headless Lua environment связывает passes;
- pass видит только объявленные typed inputs;
- artifacts получают seed/version/hash;
- provenance связывает output с pass/tool/input fingerprints;
- serial и MT execution дают одинаковый canonical result;
- cache hit не меняет результат;
- invalid artifact не публикуется;
- viewer показывает fields, graphs, filters, groups и validation issues.

### Definition of Done первого среза

Lua переставляет и параметризует проходы без изменения C++. Неверная связь типов отклоняется до
execution, одинаковый seed даёт одинаковый artifact hash, а выбранная клетка/область объясняет своё
происхождение через provenance chain.

## Дополнительные playgrounds

### 7. `network_session_lab`

Маленькая статическая комната с двумя peers и dedicated server. Проверяет ENet transport, HTTPS-like
rendezvous mock, key bootstrap, encrypted UDP envelope, NAT/relay adapter seam, intents, snapshots,
reconnect и diagnostics. Не содержит большой физики или MMO replication. Первый рост — co-op pickup;
следующий — один authoritative moving object.

### 8. `planet_streaming_lab`

Небольшая условная планета или кольцо cells без финальной графики. Camera/observer движется между cells;
система строит macro regions, загружает соседние artifacts, отменяет устаревшие jobs и проверяет epochs,
cache, residency и stable package hashes. Это ранний consumer `mmo_planet_shooter`, но не MMO server.

### 9. `city_actor_lab`

Один квартал-прокси из простых blocks, несколько actors, vertical links и одна group operation. Проверяет
сложную navigation, hierarchical actor/group intent, короткий replay и scene streaming для
`bandit_in_the_shell`. Позже можно добавить skinned character, ragdoll handoff и crowd pressure, но не
в одном первом slice.

### 10. `resource_churn_lab`

Автоматически загружает, публикует, заменяет и выгружает textures, meshes, sounds, shaders и generated
artifacts. Проверяет fence/task lifetime, stable handles, artifact epochs, bindless slot reuse,
dependency invalidation и memory budgets. Визуальная часть показывает текущие slots и intentionally
missing/corrupt resources.

### 11. `localized_text_lab`

Небольшая UI-галерея с несколькими scripts/locales. Проверяет fallback, plural/select/case forms, typed
parameters, safe tags, inline icons, pseudo-localization, RTL/bidi, shaping, font fallback, wrapping,
hit spans и locale-switch invalidation. Gameplay поставляет только loc-key и typed values.

### 12. `headless_run_lab`

Универсальная оболочка автономного scenario: seed/config, fixed tick budget, statistics, failure
artifacts и comparison между builds. Первым consumer может стать простой `cardgame` player policy без
новых карт. Позже сюда подключаются generation batch runs и dedicated-server soak tests.

### 13. `audio_environment_lab`

Три соединённые комнаты с разными материалами, дверями и acoustic zones. Проверяет
`(material, action/impact, context) -> sound event`, шаги, удары, reverb/filters, obstruction, portals,
priorities и virtual voices. Может использовать ту же proxy geometry, что `PF06_submarine_light_room`, но
остаётся отдельным аудио-сценарием.

### 14. `swarm_field_lab`

Плоская карта 128×128 с 1 000 proxy creatures, двумя food areas, одной составной operation
`approach corridor → terminal area` и одной pressure front. Проверяет versioned layered fields, brush
journal, dirty recompute, group demand/allocation, flow field, congestion, hysteresis и explanation
выбранного actor. Headless и visual runs обязаны давать один canonical hash; затем количество actors
увеличивается ступенями 1k → 10k → целевой budget. В первый slice не входят production animation,
физиология улья и полноценный бой.

### 15. `commander_mission_lab`

Граф 8–12 tactical areas, четыре абстрактных бойца, укреплённый прямой путь, обход, подготовленная
оборонительная позиция, одна неизвестная угроза, дополнительная цель и deadline эвакуации. Planner
получает только immutable squad-knowledge snapshot. Scenario проверяет mission/route plan records,
evidence-backed explanation, один interrupt, partial replan, reservation cleanup, недостижимый intent и
batch no-progress/cycle detector. Сначала это headless simulation + 2D graph/timeline inspector; 3D не нужен.

### 16. `apates_campaign_bridge_lab`

Фиксированный замкнутый граф 12–20 провинций, один persistent ruler/hero, несколько knowledge holders,
одна институциональная rule chain и одно encounter. Проверяет calendar workflow, rumor/contact/mapped-route
views, truthful rule preview и lifecycle `reserve → materialize → outcome journal → atomic reconcile`.
Первая encounter execution может быть headless/discrete: важно вернуть смерть/рану/время/предмет/
witness ровно один раз. Save/resume и different-worker runs не должны менять политический outcome.

### 17. `globe_topology_lab`

Отдельный от campaign bridge стенд замкнутой поверхности. Создаёт маленький immutable world package с
surface cells/provinces, adjacency и несколькими routes. Проверяет seam/pole-safe distance, полный обход,
projection switching, ray→surface→province picking, map-mode buffers, label anchors и terra-incognita,
которая не раскрывает hidden boundaries через hover/picking. После topology proof сюда добавляются
generator provenance и culture/history layers; distributed MMO coordinates не входят в первый scope.

## Как playground растёт в срез игры

Расширение должно идти не «добавим ещё контента», а последовательными доказанными рисками:

```text
isolated engine contract
        ↓
project-specific consumer
        ↓
second consumer / generalization review
        ↓
persistence + diagnostics + failure paths
        ↓
small repeatable gameplay loop
        ↓
early game slice
```

Перед каждым расширением стоит ответить:

1. Какой новый технический вопрос появляется?
2. Нельзя ли проверить его меньшим fixture?
3. Какая часть остаётся project-owned?
4. Что после proof действительно переносится в engine?
5. Как воспроизвести failure и сравнить результат между запусками?

Если следующий шаг отвечает только «нужно больше контента», playground уже выполнил движковую задачу.
Дальнейшее развитие должно происходить в проекте либо ждать возвращения интереса к его gameplay.

## Физическая организация

Площадки каталогизируются по коду и человеческому имени. У каждой есть собственная директория и README;
наличие отдельного executable определяется её topology и независимостью эксперимента:

```text
subprojects/
  playgrounds/
    common/
    AU01_spatial_audio/
    AU02_directional_coloration/
    PF01_forward_plus/
    PF02_shadows/
    PF03_post_processing/
    PF04_stencil_effects/
    PF05_scene_effects/
    PF06_submarine_light_room/
    PF07_party_environment/
    PF08_weather_effects/
    PF09_territory_zoning/
    PF10_functional_planet/  # README-only до первого executable
    ...
```

`common` — не mega-demo и не источник feature inheritance. Он содержит только маленькую shared shell.
Отдельный executable оправдан независимым экспериментом, отличающейся thread/process topology,
renderer presence, dedicated-server режимом или набором platform dependencies.
