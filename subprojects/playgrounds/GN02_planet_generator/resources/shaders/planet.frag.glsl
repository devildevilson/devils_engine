#version 450

// ФРАГМЕНТ САМ НАХОДИТ СВОЮ КЛЕТКУ, и в этом всё дело.
//
// Метка области дискретна: цвет провинции — это её ИМЯ, и смесь двух имён не значит ничего. Поэтому
// заливка обязана быть плоской, а гладкой должна быть её ФОРМА. На вершине это неразрешимо, и не по
// недосмотру: одно число на вершину не может назвать область, а передача «двух цветов со знаковым
// полем» разбивается о вершины в глубине области — у них пары нет, знак не согласован с соседями, и
// на карте появляются обведённые лоскуты внутри провинций и градиенты по треугольникам.
//
// У пикселя же есть своё направление, а «какая клетка здесь» — вопрос о ближайшей точке решётки,
// который можно задать в любом месте. Тогда и заливка, и её граница, и берег, и подсветка выделения
// считаются ОДНИМ полем на пиксель, и сетка треугольников не участвует в них вовсе: она осталась
// только формой поверхности.
layout(set = 0, binding = 0, std140) uniform CameraBlock {
  mat4 view_projection;
  mat4 inverse_view_projection;
  mat4 planet_to_world;
  vec4 camera_position;
  vec4 light_direction;
  vec4 params;
  vec4 viewport_near;
} camera;

layout(set = 0, binding = 2, std430) readonly buffer CellVisuals { vec4 cells[]; } visuals;
layout(set = 0, binding = 3, std430) readonly buffer CellGeometry { vec4 cells[]; } geometry;

layout(location = 0) in vec3 in_normal;
layout(location = 1) in vec3 in_direction;
layout(location = 2) in float in_height;
layout(location = 3) flat in uint in_start_cell;

layout(location = 0) out vec4 out_colour;

const uint max_neighbours = 8u;
const uint max_candidates = 9u;

// ШИРИНА ЯДРА в шагах решётки, и это главное число этого шейдера.
//
// Оно решает, на каком масштабе сглаживается граница области, и упирается в теорему, а не в
// подстройку: сгладить границу на масштабе R и сохранить подробности мельче R одновременно нельзя.
// Узкое ядро (вес обратно расстоянию, как у непрерывных полей) даёт границу ровно по серединной
// поверхности между двумя клетками — то есть ЛОМАНУЮ решётки Фибоначчи с острыми углами в точках,
// равноудалённых от трёх клеток. Это и есть «множество мелких острых кусочков».
//
// Ядро КОМПАКТНОЕ, а не гауссово, и это не украшение. Гауссово ядро приходится обрезать по кольцу
// соседей, а обрезание не симметрично относительно границы: у дальней области отрезается больше, чем
// у ближней, и граница уезжает наружу — то есть возвращается прежняя ошибка «клетка вместо мира». У
// (1 - u^2)^2 вес обращается в нуль ровно на R, и при R меньше расстояния до второго кольца
// обрезание становится ТОЧНЫМ, а не приближённым.
//
// Значение ВЫВЕДЕНО из требования «ни одна клетка не пропадает с карты». Одноклеточная область
// обязана выигрывать в своём же центре у худшего возможного кольца: восемь соседей на минимальном
// расстоянии, то есть ровно на шаге решётки. Отсюда 8*(1 - 1/R^2)^2 < 1, значит R < 1.244, и 1.2
// оставляет запас в четверть: при нём худшее кольцо набирает 0.75 против единицы у самой клетки.
//
// Первая попытка ставила 1.5 «на глаз» и была неверна арифметически: доля одиночной клетки при ней
// равна 1/(1 + 6*0.31) = 0.35, а не 0.54, как я посчитал. Одноклеточные острова при 1.5 исчезают с
// карты целиком — а они здесь не шум, они получены физикой, названы и составляют архипелаги.
const float kernel_width = 1.2;

vec3 cell_direction(const uint index) { return geometry.cells[index * 3u].xyz; }

// Число соседей лежит ЦЕЛЫМ в четвёртом слове, и читать его надо разрядами, а не приведением типа:
// целое 6 в разрядах float — это денормал, то есть нуль. С таким нулём окрестность оказывалась пустой,
// спуск не шагал ни разу, и картинка возвращалась к ближайшей клетке — ровно к решётке Вороного, из-за
// которой всё и началось. Ошибка бесшумная: ни компилятор, ни слой проверки о ней не скажут.
uint cell_neighbour_count(const uint index) {
  return min(floatBitsToUint(geometry.cells[index * 3u].w), max_neighbours);
}

uint cell_neighbour(const uint index, const uint slot) {
  const vec4 word = geometry.cells[index * 3u + 1u + (slot >> 2u)];
  return floatBitsToUint(word[slot & 3u]);
}

void main() {
  const vec3 direction = normalize(in_direction);

  // СПУСК ПО ГРАФУ от клетки, которую назвала вершина. Ячейки Вороного выпуклы, поэтому шаг «перейти
  // к ближайшему соседу» не застревает в ложном минимуме, а треугольник мельче клетки — значит и
  // шаг нужен один. Без спуска фрагмент унаследовал бы клетку вершины, то есть ту же сетку
  // треугольников, от которой всё и началось.
  uint current = in_start_cell;
  {
    float best = distance(cell_direction(current), direction);
    const uint count = cell_neighbour_count(in_start_cell);
    for (uint k = 0u; k < count; ++k) {
      const uint candidate = cell_neighbour(in_start_cell, k);
      const float value = distance(cell_direction(candidate), direction);
      if (value < best) {
        current = candidate;
        best = value;
      }
    }
  }

  // Окрестность: сама клетка и её кольцо. Дальше идти незачем, и это следствие компактного ядра:
  // второе кольцо лежит примерно в 1.7 шага решётки, то есть за нулём веса.
  uint candidates[max_candidates];
  float distances[max_candidates];
  float weights[max_candidates];
  const vec3 centre = cell_direction(current);
  const uint ring = cell_neighbour_count(current);
  candidates[0] = current;
  distances[0] = distance(centre, direction);
  uint count = 1u;
  // ШАГ РЕШЁТКИ измеряется НА МЕСТЕ — по ближайшему соседу этой клетки, а не берётся из числа клеток.
  // Соседи в записи отсортированы по расстоянию, поэтому первый и есть ближайший. Решётка Фибоначчи
  // неоднородна в мелочах, и локальная мера избавляет от подгонки константы под разрешение.
  float spacing = 1.0;
  for (uint k = 0u; k < ring; ++k) {
    const uint candidate = cell_neighbour(current, k);
    const vec3 position = cell_direction(candidate);
    if (k == 0u) {
      spacing = max(distance(position, centre), 1e-6);
    }
    candidates[count] = candidate;
    distances[count] = distance(position, direction);
    ++count;
  }

  const float radius = spacing * kernel_width;
  vec4 tails[max_candidates];
  float total = 0.0;
  for (uint k = 0u; k < count; ++k) {
    const float ratio = min(distances[k] / radius, 1.0);
    const float falloff = 1.0 - ratio * ratio;
    weights[k] = falloff * falloff;
    tails[k] = visuals.cells[candidates[k] * 2u + 1u];
    total += weights[k];
  }
  for (uint k = 0u; k < count; ++k) {
    weights[k] /= max(total, 1e-8);
  }

  vec3 base = vec3(0.0);
  // ЗАПАС ведущей области над лучшей из остальных: расстояние до границы заливки в единицах
  // покрытия, нуль — ровно граница. Обводить надо именно его, а не размытый рубеж по клеткам: у
  // рубежа нет стороны, он стоит по обе стороны границы, и его уровень 0.5 лежал на ВНЕШНЕМ краю
  // полосы — отсюда и были «полосы в стороне от границ», по одной с каждой стороны.
  float margin = 2.0;

  if (camera.params.y > 0.5) {
    // ПОКРЫТИЕ ОБЛАСТИ: сумма весов её клеток в окрестности пикселя. Заливка берёт ведущую область
    // ЦЕЛИКОМ, без смеси, поэтому цвет остаётся именем, а не превращается в третий цвет; гладкость
    // же приходит от ширины ядра, а не от размазывания цвета по треугольнику.
    uint best = 0u;
    float best_coverage = -1.0;
    float rival = 0.0;
    for (uint k = 0u; k < count; ++k) {
      float coverage = 0.0;
      for (uint j = 0u; j < count; ++j) {
        if (tails[j].x == tails[k].x) {
          coverage += weights[j];
        }
      }
      if (coverage > best_coverage) {
        best_coverage = coverage;
        best = k;
      }
    }
    for (uint k = 0u; k < count; ++k) {
      if (tails[k].x != tails[best].x) {
        float coverage = 0.0;
        for (uint j = 0u; j < count; ++j) {
          if (tails[j].x == tails[k].x) {
            coverage += weights[j];
          }
        }
        rival = max(rival, coverage);
      }
    }
    margin = best_coverage - rival;
    base = visuals.cells[candidates[best] * 2u].rgb;
  } else {
    // Непрерывное поле ИНТЕРПОЛИРУЕТСЯ, и вес здесь ДРУГОЙ — обратный расстоянию, а не компактное
    // ядро. Это не два способа сделать одно: МЕТКЕ нужна граница, ЧИСЛУ нужна интерполяция.
    //
    // Компактное ядро шириной 1.2 шага решётки узкое по построению (его ширина выведена из условия
    // «ни одна клетка не пропадает»), поэтому вес соседа при нём не больше 0.09, и как интерполяция
    // оно почти совпадает с ближайшей клеткой: карта выходила блоками по клеткам. Вес 1/r
    // обращается в бесконечность в центре клетки и плавно спадает между центрами, поэтому
    // непрерывное поле остаётся непрерывным.
    //
    // Степени у веса больше нет, и это не упрощение ради строчки. Степень нужна была, чтобы мягко
    // смешивать КЛАССЫ (климат, области рельефа), а мягкая смесь класса и есть ошибка: она даёт
    // блоки по клеткам с размытым краем. Классы теперь заливаются плоско, как и метки, а здесь
    // остались только настоящие числа — им хватает линейной интерполяции.
    float shaped[max_candidates];
    float shaped_total = 0.0;
    for (uint k = 0u; k < count; ++k) {
      // Малое слагаемое в знаменателе не косметика: пиксель может лежать точно в центре клетки, и
      // деление на нуль дало бы NaN на всю поверхность.
      shaped[k] = spacing / (distances[k] + 1e-4 * spacing);
      shaped_total += shaped[k];
    }

    // БЕРЕГ — уровень 0.5 гладкой ДОЛИ суши. Смешивать ЧЕРЕЗ берег нельзя: смесь синего с зелёным
    // даёт бирюзовый, которого нет ни у моря, ни у суши, и берег расплывается в полосу. Поэтому цвет
    // собирается по ОДНУ сторону берега, а сторону выбирает доля суши — резкий берег на гладкой
    // кривой, как на картах Земли. Класс клетки лежит ДАННЫМИ: высота обрезана нулём на воде, и
    // «высота больше нуля» у смеси означало бы «рядом есть суша», а не «здесь суша».
    float landness = 0.0;
    for (uint k = 0u; k < count; ++k) {
      landness += weights[k] * tails[k].y;
    }
    const bool split = camera.params.w > 0.5;
    const float wanted = landness > 0.5 ? 1.0 : 0.0;

    float used = 0.0;
    for (uint k = 0u; k < count; ++k) {
      if (split && tails[k].y != wanted) {
        continue;
      }
      base += visuals.cells[candidates[k] * 2u].rgb * shaped[k];
      used += shaped[k];
    }
    // Если по нужную сторону берега клеток не оказалось вовсе, берётся общая смесь: делить нечего.
    if (used <= 1e-8) {
      for (uint k = 0u; k < count; ++k) {
        base += visuals.cells[candidates[k] * 2u].rgb * shaped[k];
      }
      used = shaped_total;
    }
    base /= max(used, 1e-8);
  }

  const vec3 normal = normalize(in_normal);
  const float diffuse = max(dot(normal, -camera.light_direction.xyz), 0.0);
  // Терминатор смягчён: у глобуса он занимает половину видимого диска, и жёсткая граница читается
  // как дефект отрисовки, а не как ночь.
  const float shading = 0.42 + 0.70 * smoothstep(0.0, 0.35, diffuse) * (0.35 + 0.65 * diffuse);

  const vec3 view_direction = normalize(camera.camera_position.xyz);
  const float rim = pow(1.0 - max(dot(normal, view_direction), 0.0), 3.0);
  vec3 colour = base * shading + vec3(0.10, 0.16, 0.26) * rim * 0.55;

  // ЛИНИЯ ГРАНИЦЫ. Лежит на нуле того же запаса, по которому выбрана заливка, поэтому она не может
  // оказаться в стороне от стыка цветов.
  //
  // Рисуется не всегда (`params.y` равен двум): у климата и областей рельефа палитра смысловая, цвет
  // сам называет класс, и чёрная линия между пустыней и степью сообщала бы о резком рубеже, которого
  // в природе нет.
  if (camera.params.y > 1.5) {
    // Расстояние до границы в ПИКСЕЛЯХ. Толщина линии обязана задаваться на экране, а не в единицах
    // поля: поле меняется тем быстрее, чем дальше камера, и линия постоянной «толщины по полю»
    // расползалась у горизонта в полосу шириной с область. Именно так и получилась «грязь».
    // Наклон берётся суммой модулей, а не длиной вектора: на кромке квада одна из производных
    // обращается в нуль, и по длине линия начинала рваться на штрихи.
    const float slope = max(abs(dFdx(margin)) + abs(dFdy(margin)), 1e-8);
    const float line = 1.0 - smoothstep(0.5, 1.6, margin / slope);
    // Уровень детализации, и он про ЧИТАЕМОСТЬ, а не про скорость: когда единица покрытия занимает
    // меньше пикселя, области мельче самой линии, и линия закрашивает всё, что должна была
    // разделить. На общем виде планеты (клетка около двух пикселей) множитель равен единице, и
    // границы видны; гаснут они только при сильном отдалении.
    const float readable = smoothstep(0.4, 1.2, 1.0 / slope);
    colour = mix(colour, colour * 0.18, line * readable);
  }

  // ВЫДЕЛЕНИЕ. Не заливка чужим цветом, а подсветка своего: цвет области — это её имя на карте, и
  // подменять его на «цвет выделения» значит терять то, что выделили. Край считается тем же
  // покрытием, что и граница, поэтому подсветка ложится ровно по стыку цветов.
  const float selection = camera.light_direction.w;
  if (selection > 0.5) {
    // Край подсветки считается ТЕМ ЖЕ запасом, что и граница: покрытие выделенной области минус
    // лучшее покрытие остальных. Иначе подсветка ложится не по стыку цветов, а рядом с ним, и это
    // видно сразу — «граница и поверхность под ней не совпадают по способу отображения».
    float selected = 0.0;
    float rival = 0.0;
    for (uint k = 0u; k < count; ++k) {
      if (tails[k].x == selection) {
        selected += weights[k];
        continue;
      }
      float coverage = 0.0;
      for (uint j = 0u; j < count; ++j) {
        if (tails[j].x == tails[k].x) {
          coverage += weights[j];
        }
      }
      rival = max(rival, coverage);
    }
    const float edge = selected - rival;
    const float slope = max(abs(dFdx(edge)) + abs(dFdy(edge)), 1e-8);
    const float inside = smoothstep(-0.7, 0.7, edge / slope);
    colour = mix(colour, min(colour * 1.55 + vec3(0.14, 0.14, 0.10), vec3(1.0)), inside);
  }

  out_colour = vec4(colour, 1.0);
}
