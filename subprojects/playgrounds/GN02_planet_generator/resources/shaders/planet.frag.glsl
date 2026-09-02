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
  vec4 params;        // масштаб рельефа, режим областей (0 нет, 1 есть, 2 есть с линией), радиус, берег
  vec4 viewport_near; // ширина, высота, ближняя плоскость, ШИРИНА ЯДРА заливки
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

// ШИРИНА ЯДРА в шагах решётки, и это главное число этой карты.
//
// Оно решает, на каком масштабе сглаживается граница области, и упирается в теорему, а не в
// подстройку: сгладить границу на масштабе R и сохранить подробности мельче R одновременно нельзя.
// Узкое ядро (вес обратно расстоянию, как у непрерывных полей) даёт границу ровно по серединной
// поверхности между двумя клетками — то есть ЛОМАНУЮ решётки Фибоначчи с острыми углами в точках,
// равноудалённых от трёх клеток.
//
// Ядро КОМПАКТНОЕ — (1 - u^2)^2 с нулём ровно на радиусе, — а не гауссово, и это не украшение.
// Гауссово ядро приходится обрезать по кольцу соседей, а обрезание не симметрично относительно
// границы: у дальней области отрезается больше, чем у ближней, и граница уезжает наружу, то есть
// возвращается прежняя ошибка «клетка вместо мира». У компактного при R меньше расстояния до второго
// кольца обрезание становится ТОЧНЫМ, а не приближённым, и окрестности из клетки с кольцом хватает.
//
// Ширина ПРИХОДИТ ИЗВНЕ (`camera.viewport_near.w`), потому что это вопрос вкуса в известных
// границах: меньше — угловатее, ближе к решётке; больше — плавнее. Сами границы не вкусовые. Сверху
// держит требование «ни одна клетка не пропадает с карты»: одноклеточная область обязана выигрывать
// в своём же центре у худшего возможного кольца — восьми соседей на минимальном расстоянии, то есть
// ровно на шаге решётки, — откуда 8*(1 - 1/R^2)^2 < 1 и R < 1.244. Снизу 0.7: там сглаживания уже нет
// вовсе, граница совпадает с ломаной решётки.
//
// Первая попытка ставила 1.5 «на глаз» и была неверна АРИФМЕТИЧЕСКИ: доля одиночной клетки при ней
// равна 1/(1 + 6*0.31) = 0.35, а не 0.54, как я посчитал. Одноклеточные острова при 1.5 исчезают с
// карты целиком — а они здесь не шум, они получены физикой, названы и составляют архипелаги.
const float min_kernel_width = 0.70;
const float max_kernel_width = 1.24;

// Полуширина линии границы в пикселях. Линия рисуется от -0.5 до +0.5 пикселя вокруг неё, то есть
// занимает примерно полтора пикселя с мягким краем — как в PF10.
const float line_half_width = 0.6;

vec3 cell_direction(const uint index) { return geometry.cells[index * 3u].xyz; }

// Число соседей лежит ЦЕЛЫМ в четвёртом слове, и читать его надо разрядами, а не приведением типа:
// целое 6 в разрядах float — это денормал, то есть нуль. С таким нулём окрестность оказывалась
// пустой, спуск не шагал ни разу, и картинка возвращалась к ближайшей клетке — ровно к решётке
// Вороного, из-за которой всё и началось. Ошибка бесшумная: ни компилятор, ни слой проверки о ней не
// скажут.
uint cell_neighbour_count(const uint index) {
  return min(floatBitsToUint(geometry.cells[index * 3u].w), max_neighbours);
}

uint cell_neighbour(const uint index, const uint slot) {
  const vec4 word = geometry.cells[index * 3u + 1u + (slot >> 2u)];
  return floatBitsToUint(word[slot & 3u]);
}

void main() {
  const vec3 direction = normalize(in_direction);
  // Производные НАПРАВЛЕНИЯ поверхности берутся один раз и до всяких ветвлений. Направление гладко
  // по построению, поэтому его производная — честная мера «сколько поверхности приходится на один
  // пиксель», и на ней держатся и толщина линии, и сглаживание края.
  const vec3 pixel_x = dFdx(direction);
  const vec3 pixel_y = dFdy(direction);

  // СПУСК ПО ГРАФУ от клетки, которую назвала вершина. Ячейки Вороного выпуклы, поэтому шаг «перейти
  // к ближайшему соседу» не застревает в ложном минимуме, а треугольник мельче клетки — значит и
  // шага нужно одного. Без спуска фрагмент унаследовал бы клетку вершины, то есть ту же сетку
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
  vec3 positions[max_candidates];
  float distances[max_candidates];
  float weights[max_candidates];
  float areas[max_candidates];
  float land[max_candidates];

  const vec3 centre = cell_direction(current);
  const uint ring = cell_neighbour_count(current);
  candidates[0] = current;
  positions[0] = centre;
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
    positions[count] = position;
    distances[count] = distance(position, direction);
    ++count;
  }

  const float radius = spacing * clamp(camera.viewport_near.w, min_kernel_width, max_kernel_width);
  // Вес и ЕГО ПРОИЗВОДНАЯ считаются одним проходом: у них общее отношение расстояния к радиусу и
  // общее деление на расстояние, а проход по окрестности — самое дорогое место этого шейдера.
  //
  // Производная нужна в явном виде, и это ответ на «дырявые границы». Экранная производная от
  // покрытия негодна принципиально: набор клеток окрестности меняется, когда пиксель переходит из
  // одной ячейки Вороного в другую, и покрытие получает маленький скачок. Сам скачок глазу не виден
  // (граница смещается на сотые доли клетки), но ПРОИЗВОДНАЯ в этом месте улетает, а по ней считалась
  // толщина линии — линия то худела до нуля, то расползалась, и выходила РВАНОЙ. У ядра производная
  // есть аналитически: d/dr (1 - u^2)^2 = -4u(1 - u^2)/R, и она непрерывна.
  vec3 kernel_gradient[max_candidates];
  vec3 gradient_total = vec3(0.0);
  float total = 0.0;
  for (uint k = 0u; k < count; ++k) {
    const float ratio = min(distances[k] / radius, 1.0);
    const float falloff = 1.0 - ratio * ratio;
    // Веса остаются СЫРЫМИ, а сумма держится отдельно: нормировать их сразу нельзя, потому что по
    // этой же сумме считается производная покрытия, а у нормированной величины она другая.
    weights[k] = falloff * falloff;
    const float slope = -4.0 * ratio * falloff / radius;
    kernel_gradient[k] = slope * (direction - positions[k]) / max(distances[k], 1e-7);
    gradient_total += kernel_gradient[k];
    const vec4 tail = visuals.cells[candidates[k] * 2u + 1u];
    areas[k] = tail.x;
    land[k] = tail.y;
    total += weights[k];
  }
  const float inverse_total = 1.0 / max(total, 1e-8);

  // Перевод «значение поля» в «расстояние в пикселях». Радиальную часть градиента надо убрать:
  // пиксель ходит ПО СФЕРЕ, и движение вдоль радиуса никуда его не переносит.
  const float pixel_angle = max(max(length(pixel_x), length(pixel_y)), 1e-9);

  vec3 base = vec3(0.0);
  // Расстояние до границы заливки В ПИКСЕЛЯХ; большое значение означает «границы рядом нет». Именно
  // пиксели, а не единицы покрытия: и линия, и сглаживание края обязаны иметь ЭКРАННУЮ толщину,
  // иначе они меняются вместе с расстоянием до камеры.
  float border_pixels = 1e6;
  float selected_fraction = 0.0;

  if (camera.params.y > 0.5) {
    // ПОКРЫТИЕ ОБЛАСТИ: сумма весов её клеток в окрестности пикселя. Заливка берёт ведущую область
    // ЦЕЛИКОМ, без смеси, поэтому цвет остаётся именем, а не превращается в третий цвет; гладкость
    // же приходит от ширины ядра, а не от размазывания цвета по треугольнику.
    float coverage[max_candidates];
    for (uint k = 0u; k < count; ++k) {
      float sum = 0.0;
      for (uint j = 0u; j < count; ++j) {
        if (areas[j] == areas[k]) {
          sum += weights[j];
        }
      }
      coverage[k] = sum;
    }

    uint best = 0u;
    for (uint k = 1u; k < count; ++k) {
      if (coverage[k] > coverage[best]) {
        best = k;
      }
    }
    // Соперник — лучшая из ОСТАЛЬНЫХ областей. Он нужен не только для запаса: его цвет идёт в
    // сглаживание края, потому что на границе пиксель накрыт двумя областями.
    bool has_rival = false;
    uint rival = 0u;
    for (uint k = 0u; k < count; ++k) {
      if (areas[k] != areas[best] && (!has_rival || coverage[k] > coverage[rival])) {
        has_rival = true;
        rival = k;
      }
    }

    base = visuals.cells[candidates[best] * 2u].rgb;

    if (has_rival) {
      vec3 gradient_best = vec3(0.0);
      vec3 gradient_rival = vec3(0.0);
      for (uint k = 0u; k < count; ++k) {
        if (areas[k] == areas[best]) {
          gradient_best += kernel_gradient[k];
        } else if (areas[k] == areas[rival]) {
          gradient_rival += kernel_gradient[k];
        }
      }
      // Покрытие нормировано общей суммой, поэтому производная считается по правилу частного. Второе
      // слагаемое мало у самой границы (там покрытия равны), но у него нет причин исчезать вдали от
      // неё, а расстояние считается по всему полю.
      const float difference = coverage[best] - coverage[rival];
      const vec3 gradient = (gradient_best - gradient_rival) * inverse_total -
                            difference * gradient_total * inverse_total * inverse_total;
      const vec3 tangential = gradient - direction * dot(gradient, direction);
      const float steepness = length(tangential);
      if (steepness > 1e-7) {
        const vec3 across = tangential / steepness;
        // Сколько по ЭТОЙ оси проходит один пиксель. Ось своя у каждой границы, поэтому и толщина
        // линии не зависит от того, под каким углом граница стоит к экрану.
        const float step = max(length(vec2(dot(pixel_x, across), dot(pixel_y, across))), 1e-9);
        border_pixels = difference * inverse_total / (steepness * step);
      }

      // СГЛАЖИВАНИЕ КРАЯ ЗАЛИВКИ. На границе пиксель накрыт двумя областями, и закрашивать его
      // целиком цветом ведущей — значит рисовать лестницу в один пиксель. Именно она и читалась как
      // «дырявые границы»: тонкая линия поверх лестницы то попадала в пиксель, то нет.
      //
      // Это НЕ запрещённое смешивание метк: смешиваются не значения областей, а их ДОЛИ В ПЛОЩАДИ
      // ПИКСЕЛЯ — ровно как у буквы в шрифте. Смесь живёт в один пиксель шириной и третьего цвета на
      // карте не создаёт.
      const vec3 rival_colour = visuals.cells[candidates[rival] * 2u].rgb;
      base = mix(rival_colour, base, clamp(border_pixels + 0.5, 0.0, 1.0));
    }

    // ВЫДЕЛЕНИЕ считается тем же расстоянием, что и граница, поэтому подсветка ложится ровно по стыку
    // цветов: пока они считались по-разному, было видно, что «граница и поверхность под ней не
    // совпадают по способу отображения».
    const float selection = camera.light_direction.w;
    if (selection > 0.5) {
      if (areas[best] == selection) {
        selected_fraction = clamp(border_pixels + 0.5, 0.0, 1.0);
      } else if (has_rival && areas[rival] == selection) {
        selected_fraction = clamp(0.5 - border_pixels, 0.0, 1.0);
      }
    }
  } else {
    // Непрерывное поле ИНТЕРПОЛИРУЕТСЯ, и вес здесь ДРУГОЙ — обратный расстоянию, а не компактное
    // ядро. Это не два способа сделать одно: МЕТКЕ нужна граница, ЧИСЛУ нужна интерполяция.
    //
    // Компактное ядро узкое по построению (его ширина выведена из условия «ни одна клетка не
    // пропадает»), поэтому вес соседа при нём не больше 0.09, и как интерполяция оно почти совпадает
    // с ближайшей клеткой: карта выходила блоками по клеткам. Вес 1/r обращается в бесконечность в
    // центре клетки и плавно спадает между центрами, поэтому непрерывное поле остаётся непрерывным.
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

    if (camera.params.w > 0.5) {
      // БЕРЕГ — уровень 0.5 гладкой ДОЛИ суши. Смешивать ЧЕРЕЗ берег нельзя: смесь синего с зелёным
      // даёт бирюзовый, которого нет ни у моря, ни у суши, и берег расплывается в полосу. Поэтому
      // цвет собирается РАЗДЕЛЬНО по сторонам, а сторону выбирает доля суши — резкий берег на
      // гладкой кривой, как на картах Земли. Класс клетки лежит ДАННЫМИ: высота обрезана нулём на
      // воде, и «высота больше нуля» у смеси означало бы «рядом есть суша», а не «здесь суша».
      vec3 land_colour = vec3(0.0);
      vec3 water_colour = vec3(0.0);
      float land_weight = 0.0;
      float water_weight = 0.0;
      float land_sum = 0.0;
      vec3 land_gradient = vec3(0.0);
      for (uint k = 0u; k < count; ++k) {
        const vec3 colour = visuals.cells[candidates[k] * 2u].rgb;
        if (land[k] > 0.5) {
          land_colour += colour * shaped[k];
          land_weight += shaped[k];
          land_sum += weights[k];
          land_gradient += kernel_gradient[k];
        } else {
          water_colour += colour * shaped[k];
          water_weight += shaped[k];
        }
      }
      // Доля суши считается КОМПАКТНЫМ ядром, а не весом 1/r: берег — граница метки, и ему нужна та
      // же гладкая форма, что и границам областей, включая сохранность одноклеточных островов.
      const float landness = land_sum * inverse_total;
      const vec3 gradient = (land_gradient - landness * gradient_total) * inverse_total;
      const vec3 tangential = gradient - direction * dot(gradient, direction);
      const float steepness = length(tangential);

      float coast_pixels = landness > 0.5 ? 1e6 : -1e6;
      if (steepness > 1e-7) {
        const vec3 across = tangential / steepness;
        const float step = max(length(vec2(dot(pixel_x, across), dot(pixel_y, across))), 1e-9);
        coast_pixels = (landness - 0.5) / (steepness * step);
      }

      // Если по одну сторону берега клеток не оказалось вовсе, у неё нет и цвета: берётся другая.
      const vec3 dry = land_weight > 1e-8 ? land_colour / land_weight : water_colour / max(water_weight, 1e-8);
      const vec3 wet = water_weight > 1e-8 ? water_colour / water_weight : dry;
      base = mix(wet, dry, clamp(coast_pixels + 0.5, 0.0, 1.0));
    } else {
      for (uint k = 0u; k < count; ++k) {
        base += visuals.cells[candidates[k] * 2u].rgb * shaped[k];
      }
      base /= max(shaped_total, 1e-8);
    }
  }

  const vec3 normal = normalize(in_normal);
  const float diffuse = max(dot(normal, -camera.light_direction.xyz), 0.0);
  // Терминатор смягчён: у глобуса он занимает половину видимого диска, и жёсткая граница читается
  // как дефект отрисовки, а не как ночь.
  const float shading = 0.42 + 0.70 * smoothstep(0.0, 0.35, diffuse) * (0.35 + 0.65 * diffuse);

  const vec3 view_direction = normalize(camera.camera_position.xyz);
  const float rim = pow(1.0 - max(dot(normal, view_direction), 0.0), 3.0);
  vec3 colour = base * shading + vec3(0.10, 0.16, 0.26) * rim * 0.55;

  // ЛИНИЯ ГРАНИЦЫ. Лежит на нуле того же расстояния, по которому выбрана заливка и сглажен её край,
  // поэтому она не может оказаться в стороне от стыка цветов.
  //
  // Рисуется не всегда (`params.y` равен двум): у климата и областей рельефа палитра смысловая, цвет
  // сам называет класс, и чёрная черта между пустыней и степью сообщала бы о рубеже, которого в
  // природе нет.
  if (camera.params.y > 1.5) {
    const float line = 1.0 - smoothstep(line_half_width - 0.5, line_half_width + 0.5, border_pixels);
    // Уровень детализации, и он про ЧИТАЕМОСТЬ, а не про скорость: когда на клетку приходится меньше
    // пикселя, области мельче самой линии, и линия закрашивает всё, что должна была разделить. На
    // общем виде планеты (клетка около двух пикселей) множитель равен единице, гаснет он только при
    // сильном отдалении.
    const float readable = smoothstep(0.6, 1.6, spacing / pixel_angle);
    colour = mix(colour, colour * 0.18, line * readable);
  }

  // ВЫДЕЛЕНИЕ. Не заливка чужим цветом, а подсветка своего: цвет области — это её имя на карте, и
  // подменять его на «цвет выделения» значит терять то, что выделили.
  if (selected_fraction > 0.0) {
    colour = mix(colour, min(colour * 1.55 + vec3(0.14, 0.14, 0.10), vec3(1.0)), selected_fraction);
  }

  out_colour = vec4(colour, 1.0);
}
