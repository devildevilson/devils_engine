#include "visual.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>

#include <glm/geometric.hpp>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/kd_tree.h"

namespace devils_engine::gn02 {

namespace {

using tree_point = std::array<float, 3>;
using cell_tree = utils::kd_tree<uint32_t, tree_point, 3>;

// Режимы просмотра. Порядок задаёт и порядок цифровых клавиш, и порядок перебора.
//
// Имена латиницей потому, что они уходят прямиком в оверлей, а атлас шрифта площадок покрывает
// только ASCII: кириллица в нём не отрисовывается вовсе.
const std::array<view_mode, 20> modes{{
  // После подсказки идут: с какого шага поле осмысленно, какой уровень названных мест ему
  // соответствует и три флага — есть ли ОБЛАСТИ (то есть плоская заливка по покрытию), обводить ли
  // границу ЛИНИЕЙ и делится ли палитра по берегу.
  //
  // Области и линия — РАЗНЫЕ решения, и разница видна на климате. У климата области есть (зона —
  // это метка клетки), поэтому заливка у него плоская. Но линии нет: палитра СМЫСЛОВАЯ — пустыня
  // песочная, лес зелёный, — цвет сам называет класс, а чёрная черта между пустыней и степью
  // сообщала бы о рубеже, которого в природе нет.
  //
  // Класс — это МЕТКА, а не число, и смешивать его нельзя даже мягко. Первая версия смешивала
  // климат и области рельефа с резкостью, и обе карты выходили бусами: у степени от веса ближайшая
  // клетка забирает почти всё, поэтому получались блоки по клеткам с мягким краем — то есть решётка
  // Фибоначчи, показанная вместо мира. Смешивание осталось только у настоящих чисел (высота,
  // температура, осадки), и там оно линейное.
  {"climate", "zones from summer, winter and rain", 5, view_mode::named::none, true},
  {"relief", "hypsometry: depths blue, land green to snow", 2, view_mode::named::none, false, false, true},
  {"plates", "tectonic plates, convergent rims warm, divergent cold", 2, view_mode::named::none, true, true},
  {"tectonics", "convergence red, divergence blue, subduction dark", 2, view_mode::named::none},
  {"temperature", "mean of summer and winter", 5, view_mode::named::none},
  {"seasonality", "summer minus winter", 5, view_mode::named::none},
  {"precipitation", "in shares of the land mean", 4, view_mode::named::none},
  {"habitability", "where a person can live", 6, view_mode::named::none},
  {"population", "density, logarithmic", 6, view_mode::named::none},
  {"cultures", "who claimed the cell", 6, view_mode::named::none, true, true},
  {"provinces", "land carved into provinces", 7, view_mode::named::province, true, true},
  {"sea zones", "water carved into zones", 7, view_mode::named::sea_zone, true, true},
  // У областей рельефа палитра тоже смысловая, и линии у них нет по той же причине, что у климата:
  // граница между холмами и горами измерена, но она не рубеж, а порог на непрерывном склоне.
  {"landforms", "what kind of place this is, and it is measured", 4, view_mode::named::landform, true},
  // Уровни географической иерархии. Красятся ТЕМ ЖЕ способом, что провинции, и это само по себе
  // проверка: если граница уровня не совпадёт с границей провинции, на карте появится пятно внутри
  // области, а не только цифра в отчёте.
  {"land masses", "connected pieces of land: Eurasia, not Europe", 9, view_mode::named::land_mass, true, true},
  {"continents", "grown inside a land mass, small islands attached", 9, view_mode::named::continent, true, true},
  {"historical regions", "grown inside a continent: Northern Europe", 9, view_mode::named::historical_region,
   true, true},
  {"oceans", "grown inside a water body; lakes have none", 9, view_mode::named::ocean, true, true},
  // Политика. Тем же способом и по той же причине: границы титула обязаны лежать по границам
  // графств, и на карте это видно сразу.
  {"duchies", "de jure duchies inside a kingdom", 9, view_mode::named::duchy, true, true},
  {"empires", "de jure empires, islands attached", 9, view_mode::named::empire, true, true},
  {"realms", "de facto states: who actually holds the land", 9, view_mode::named::realm, true, true},
}};

originator::const_field_accessor field_of(originator::pipeline& source, const std::string_view& buffer_name,
                                         const std::string_view& field_name) {
  auto* buffer = source.find_buffer(buffer_name);
  if (buffer == nullptr) {
    utils::error{}("GN02 visual: no buffer '{}'", buffer_name);
  }
  const size_t index = buffer->find_field(field_name);
  if (index == originator::buffer_layout::npos) {
    utils::error{}("GN02 visual: buffer '{}' has no field '{}'", buffer_name, field_name);
  }
  return buffer->field(index);
}

glm::vec3 ramp(const double t, const std::span<const glm::vec3>& stops) {
  if (stops.empty()) {
    return glm::vec3(0.0f);
  }
  const double clamped = std::clamp(t, 0.0, 1.0) * double(stops.size() - 1);
  const auto low = size_t(clamped);
  const auto high = std::min(low + 1, stops.size() - 1);
  const auto fraction = float(clamped - double(low));
  return stops[low] * (1.0f - fraction) + stops[high] * fraction;
}

// Цвет метки: хеш номера, а не таблица. Областей тысячи, поэтому таблицу пришлось бы генерировать, а
// хеш даёт устойчивый цвет, который не меняется от прогона к прогону при том же номере.
glm::vec3 label_colour(const uint32_t label) {
  if (label == 0) {
    return glm::vec3(0.07f, 0.08f, 0.10f);
  }
  uint64_t x = uint64_t(label) * 0x9e3779b97f4a7c15ull;
  x ^= x >> 29;
  x *= 0xbf58476d1ce4e5b9ull;
  x ^= x >> 32;

  const float hue = float(double(x & 0xffffff) / double(0x1000000)) * 6.0f;
  const float saturation = 0.45f + 0.35f * float(double((x >> 24) & 0xffff) / double(0x10000));
  const float value = 0.55f + 0.40f * float(double((x >> 40) & 0xffff) / double(0x10000));

  const float sector = std::floor(hue);
  const float fraction = hue - sector;
  const float p = value * (1.0f - saturation);
  const float q = value * (1.0f - saturation * fraction);
  const float t = value * (1.0f - saturation * (1.0f - fraction));
  switch (int(sector) % 6) {
    case 0: return {value, t, p};
    case 1: return {q, value, p};
    case 2: return {p, value, t};
    case 3: return {p, q, value};
    case 4: return {t, p, value};
    default: return {value, p, q};
  }
}

// Палитра климатических зон. Порядок совпадает с идентификаторами из scripts/S06_climate_zone.ds и с
// таблицей символов ASCII-карты: одно правило, три представления, и все три читают одну нумерацию.
const std::array<glm::vec3, climate_class_count> climate_palette{{
  {0.05f, 0.14f, 0.32f}, // 0 океан
  {0.62f, 0.72f, 0.82f}, // 1 морской лёд
  {0.92f, 0.94f, 0.97f}, // 2 ледник
  {0.55f, 0.56f, 0.48f}, // 3 тундра
  {0.16f, 0.32f, 0.20f}, // 4 тайга
  {0.20f, 0.46f, 0.18f}, // 5 лес
  {0.66f, 0.62f, 0.32f}, // 6 степь
  {0.82f, 0.72f, 0.44f}, // 7 пустыня
  {0.55f, 0.60f, 0.24f}, // 8 саванна
  {0.10f, 0.42f, 0.16f}, // 9 джунгли
  {0.78f, 0.78f, 0.80f}, // 10 высокогорье
}};

// Палитра видов областей рельефа. Порядок совпадает с идентификаторами из scripts/S04_landform_water.ds
// и scripts/S04_landform_land.ds и с таблицей имён в main.cpp.
//
// Цвета подобраны так, чтобы вид читался без легенды: под водой синие от глубокого к мелкому,
// на суше от зелёного (равнина) через охру (нагорье) к серому (горы), вулканический остров красный —
// он и есть чужая кора посреди океана.
const std::array<glm::vec3, landform_class_count> landform_palette{{
  {0.06f, 0.12f, 0.26f}, // 0 абиссальная равнина
  {0.16f, 0.34f, 0.52f}, // 1 океанический хребет
  {0.35f, 0.62f, 0.75f}, // 2 шельф
  {0.50f, 0.76f, 0.78f}, // 3 архипелаг
  {0.02f, 0.05f, 0.14f}, // 4 жёлоб
  {0.42f, 0.66f, 0.36f}, // 5 прибрежная равнина
  {0.30f, 0.52f, 0.26f}, // 6 низина
  {0.72f, 0.62f, 0.34f}, // 7 нагорье
  {0.60f, 0.55f, 0.36f}, // 8 холмы
  {0.72f, 0.72f, 0.74f}, // 9 горы
  {0.74f, 0.32f, 0.24f}, // 10 вулканический остров
}};

glm::vec3 relief_colour(const double height, const bool land) {
  if (!land) {
    const std::array<glm::vec3, 4> deep{{
      {0.02f, 0.06f, 0.18f}, {0.04f, 0.13f, 0.32f}, {0.07f, 0.24f, 0.48f}, {0.16f, 0.40f, 0.60f}}};
    // Глубина считается от нуля вниз: -9000 м и -200 м обязаны отличаться заметно, иначе шельф
    // сливается с абиссалью и «формы дна» на карте не видно вовсе.
    const double t = std::clamp(1.0 + height / 9000.0, 0.0, 1.0);
    return ramp(t, deep);
  }

  const std::array<glm::vec3, 6> ground{{
    {0.24f, 0.42f, 0.24f}, {0.42f, 0.52f, 0.26f}, {0.60f, 0.54f, 0.30f},
    {0.52f, 0.40f, 0.26f}, {0.62f, 0.58f, 0.54f}, {0.97f, 0.97f, 0.99f}}};
  // Корень, а не линейная шкала: почти вся суша лежит ниже километра, и на линейной шкале материк
  // выходит одного ровного зелёного цвета, а высоты видно только в горах.
  return ramp(std::sqrt(std::clamp(height / 6000.0, 0.0, 1.0)), ground);
}

} // namespace

std::vector<labelled_area> collect_labelled_areas(originator::pipeline& source, const view_mode::named level) {
  // Уровень задаёт БУФЕР ЗАПИСЕЙ и его поле счёта — больше ничего. Соответствие лежит таблицей, а не
  // лестницей условий, потому что вопрос ровно один: где лежат центры этого уровня.
  struct source_table {
    view_mode::named level;
    const char* records;
    const char* size_field;
    const char* count;
  };
  static constexpr source_table table[] = {
    {view_mode::named::province, "provinces", "cells", "province_count"},
    {view_mode::named::sea_zone, "sea_zones", "cells", "sea_zone_count"},
    {view_mode::named::land_mass, "land_masses", "cells", "land_mass_count"},
    {view_mode::named::continent, "continents", "cells", "continent_count"},
    {view_mode::named::historical_region, "historical_regions", "cells", "historical_count"},
    {view_mode::named::ocean, "oceans", "cells", "ocean_count"},
    {view_mode::named::duchy, "duchies", "cells", "duchy_count"},
    {view_mode::named::empire, "empires", "cells", "empire_count"},
    {view_mode::named::realm, "realms", "cells", "realm_count"},
  };

  for (const auto& entry : table) {
    if (entry.level != level) {
      continue;
    }
    const auto centres = field_of(source, entry.records, "center");
    const auto sizes = field_of(source, entry.records, entry.size_field);
    const auto total = size_t(field_of(source, "state", entry.count).get(0));

    std::vector<labelled_area> result;
    result.reserve(total);
    for (size_t i = 1; i <= total && i < sizes.count(); ++i) {
      const glm::vec3 centre{float(centres.get(i, 0)), float(centres.get(i, 1)), float(centres.get(i, 2))};
      const float length = glm::length(centre);
      if (sizes.get(i) <= 0.0 || length < 1.0e-4f) {
        continue;
      }
      result.push_back(labelled_area{centre / length, uint32_t(i), float(sizes.get(i))});
    }
    return result;
  }

  return {};
}

struct cell_locator::state {
  cell_tree tree;
  float radius = 0.1f;
};

cell_locator::cell_locator(originator::pipeline& source) : state_(std::make_unique<state>()) {
  const auto* cells = source.find_buffer("cells");
  if (cells == nullptr) {
    utils::error{}("GN02 visual: the pipeline has no 'cells' buffer");
  }
  const auto positions = field_of(source, "cells", "position");
  const size_t count = cells->count();
  state_->tree.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    state_->tree.insert(
      tree_point{float(positions.get(i, 0)), float(positions.get(i, 1)), float(positions.get(i, 2))}, uint32_t(i));
  }
  state_->tree.build();
  // Радиус поиска с запасом от шага решётки: пустой ответ означал бы «под курсором нет планеты», а
  // это неправда, если луч в неё попал.
  state_->radius = float(6.0 * std::sqrt(4.0 * std::numbers::pi / double(std::max<size_t>(count, 1))));
}

cell_locator::~cell_locator() = default;
cell_locator::cell_locator(cell_locator&&) noexcept = default;
cell_locator& cell_locator::operator=(cell_locator&&) noexcept = default;

uint32_t cell_locator::locate(const glm::vec3& direction) const {
  const glm::vec3 unit = glm::normalize(direction);
  const tree_point query{unit.x, unit.y, unit.z};
  float radius = state_->radius;
  for (uint32_t attempt = 0; attempt < 6; ++attempt) {
    const auto* node = state_->tree.nearest(query, radius, [](const uint32_t&) { return true; });
    if (node != nullptr) {
      return node->payload;
    }
    radius *= 2.0f;
  }
  return UINT32_MAX;
}

std::span<const view_mode> view_modes() noexcept {
  return std::span<const view_mode>(modes.data(), modes.size());
}

std::vector<surface_vertex> build_surface(originator::pipeline& source, const uint32_t subdivisions) {
  const auto* cells = source.find_buffer("cells");
  if (cells == nullptr) {
    utils::error{}("GN02 visual: the pipeline has no 'cells' buffer");
  }
  const auto positions = field_of(source, "cells", "position");
  const size_t count = cells->count();

  cell_tree tree;
  tree.reserve(count);
  for (size_t i = 0; i < count; ++i) {
    const tree_point point{float(positions.get(i, 0)), float(positions.get(i, 1)), float(positions.get(i, 2))};
    tree.insert(point, uint32_t(i));
  }
  tree.build();

  // Икосаэдр: двенадцать вершин, двадцать граней. Дальше каждый треугольник делится на четыре, а
  // новые вершины возвращаются на сферу нормированием — так грани остаются почти равновеликими.
  const float golden = float((1.0 + std::sqrt(5.0)) * 0.5);
  std::array<glm::vec3, 12> base{};
  size_t written = 0;
  for (const float sign_a : {-1.0f, 1.0f}) {
    for (const float sign_b : {-1.0f, 1.0f}) {
      base[written++] = glm::normalize(glm::vec3(0.0f, sign_a * 1.0f, sign_b * golden));
      base[written++] = glm::normalize(glm::vec3(sign_a * 1.0f, sign_b * golden, 0.0f));
      base[written++] = glm::normalize(glm::vec3(sign_a * golden, 0.0f, sign_b * 1.0f));
    }
  }

  // Грани икосаэдра собираются по расстоянию: у правильного икосаэдра ровно 30 рёбер минимальной
  // длины, и тройка взаимно ближайших вершин — это грань. Так не нужно держать в коде таблицу
  // индексов, в которой легко ошибиться ориентацией.
  const float edge = 2.0f / std::sqrt(golden * golden + 1.0f);
  const float threshold = edge * 1.1f;
  std::vector<std::array<glm::vec3, 3>> triangles;
  triangles.reserve(20);
  for (size_t a = 0; a < base.size(); ++a) {
    for (size_t b = a + 1; b < base.size(); ++b) {
      if (glm::distance(base[a], base[b]) > threshold) {
        continue;
      }
      for (size_t c = b + 1; c < base.size(); ++c) {
        if (glm::distance(base[a], base[c]) > threshold || glm::distance(base[b], base[c]) > threshold) {
          continue;
        }
        std::array<glm::vec3, 3> face{base[a], base[b], base[c]};
        // Наружная ориентация: нормаль треугольника обязана смотреть от центра сферы.
        if (glm::dot(glm::cross(face[1] - face[0], face[2] - face[0]), face[0]) < 0.0f) {
          std::swap(face[1], face[2]);
        }
        triangles.push_back(face);
      }
    }
  }
  if (triangles.size() != 20) {
    utils::error{}("GN02 visual: icosahedron assembly produced {} faces instead of 20", triangles.size());
  }

  const uint32_t levels = std::min(subdivisions, 8u);
  for (uint32_t level = 0; level < levels; ++level) {
    std::vector<std::array<glm::vec3, 3>> refined;
    refined.reserve(triangles.size() * 4);
    for (const auto& face : triangles) {
      const glm::vec3 ab = glm::normalize(face[0] + face[1]);
      const glm::vec3 bc = glm::normalize(face[1] + face[2]);
      const glm::vec3 ca = glm::normalize(face[2] + face[0]);
      refined.push_back({face[0], ab, ca});
      refined.push_back({ab, face[1], bc});
      refined.push_back({ca, bc, face[2]});
      refined.push_back({ab, bc, ca});
    }
    triangles.swap(refined);
  }

  // Четыре ближайшие клетки на вершину и веса по расстоянию. Радиус поиска берётся с запасом от шага
  // решётки: он должен накрыть не одну клетку, а её окружение, иначе кандидатов будет меньше четырёх
  // и сглаживать будет нечем.
  const double spacing = std::sqrt(4.0 * std::numbers::pi / double(std::max<size_t>(count, 1)));
  const float blend_radius = float(2.2 * spacing);

  std::vector<surface_vertex> result;
  result.reserve(triangles.size() * 3);
  std::vector<std::pair<float, uint32_t>> found;
  found.reserve(64);

  for (const auto& face : triangles) {
    for (const auto& direction : face) {
      const tree_point query{direction.x, direction.y, direction.z};
      found.clear();
      float radius = blend_radius;
      for (uint32_t attempt = 0; attempt < 6 && found.size() < 4; ++attempt) {
        found.clear();
        tree.radius(
          query, radius, [](const uint32_t&) { return true; },
          [&](const cell_tree::node& node) {
            const float dx = node.pos[0] - query[0];
            const float dy = node.pos[1] - query[1];
            const float dz = node.pos[2] - query[2];
            found.emplace_back(dx * dx + dy * dy + dz * dz, node.payload);
          });
        radius *= 1.7f;
      }

      if (found.empty()) {
        // Запас радиуса не сработал только если клеток вообще нет; тогда лучше упасть, чем рисовать
        // поверхность, привязанную к нулевой клетке.
        utils::error{}("GN02 visual: no cell within {} of a surface vertex", radius);
      }
      std::sort(found.begin(), found.end());

      surface_vertex vertex{};
      vertex.direction = direction;

      // Вес обратно пропорционален расстоянию, а не его квадрату: у квадрата ближайшая клетка
      // забирает почти всё, и сглаживания не выходит. Малое слагаемое в знаменателе не косметика —
      // вершина сетки может лежать точно в клетке, и тогда деление на ноль дало бы NaN на всю
      // поверхность.
      const size_t taken = std::min<size_t>(found.size(), 4);
      std::array<double, 4> weights{};
      double total = 0.0;
      for (size_t k = 0; k < taken; ++k) {
        const double distance = std::sqrt(double(found[k].first));
        weights[k] = 1.0 / (distance + 1.0e-4 * spacing);
        total += weights[k];
        vertex.cells[k] = found[k].second;
      }
      for (size_t k = taken; k < 4; ++k) {
        vertex.cells[k] = found[0].second;
      }

      // Упаковка в байты с добором остатка в первый вес: сумма обязана быть ровно 255, иначе шейдеру
      // придётся нормировать заново, а это лишняя работа на каждую вершину каждого кадра.
      std::array<uint32_t, 4> packed{};
      uint32_t written_total = 0;
      for (size_t k = 0; k < taken; ++k) {
        packed[k] = uint32_t(std::lround(255.0 * weights[k] / total));
        written_total += packed[k];
      }
      if (written_total > 255) {
        packed[0] -= std::min(packed[0], written_total - 255);
      } else {
        packed[0] += 255 - written_total;
      }
      vertex.weights = packed[0] | (packed[1] << 8) | (packed[2] << 16) | (packed[3] << 24);

      result.push_back(vertex);
    }
  }
  return result;
}

std::vector<cell_geometry> build_cell_geometry(originator::pipeline& source, const size_t cells) {
  const auto positions = field_of(source, "cells", "position");
  const auto offsets = field_of(source, "cell_offsets", "start");
  const auto arcs = field_of(source, "cell_arcs", "cell");

  std::vector<cell_geometry> result(cells);
  std::vector<std::pair<float, uint32_t>> ring;
  for (size_t i = 0; i < cells; ++i) {
    auto& record = result[i];
    record.direction = glm::vec3(float(positions.get(i, 0)), float(positions.get(i, 1)),
                                 float(positions.get(i, 2)));

    const auto first = size_t(offsets.get(i));
    const auto last = size_t(offsets.get(i + 1));
    ring.clear();
    for (size_t k = first; k < last; ++k) {
      const auto other = size_t(arcs.get(k));
      if (other >= cells) {
        continue;
      }
      const glm::vec3 direction(float(positions.get(other, 0)), float(positions.get(other, 1)),
                                float(positions.get(other, 2)));
      ring.emplace_back(glm::dot(direction - record.direction, direction - record.direction), uint32_t(other));
    }
    // Если соседей больше восьми, берутся БЛИЖАЙШИЕ: спуску нужны те, через которые он идёт, а
    // дальние соседи симметризации только занимают место.
    std::sort(ring.begin(), ring.end());
    record.neighbour_count = uint32_t(std::min<size_t>(ring.size(), record.neighbours.size()));
    for (uint32_t k = 0; k < record.neighbour_count; ++k) {
      record.neighbours[k] = ring[k].second;
    }
  }
  return result;
}

std::vector<cell_visual> build_cell_visuals(originator::pipeline& source, const size_t mode, const size_t cells) {
  const auto height = field_of(source, "cells", "height");
  const auto land = field_of(source, "cells", "land");
  const auto sea_level = field_of(source, "state", "sea_level").get(0);

  std::vector<cell_visual> result(cells);
  for (size_t i = 0; i < cells; ++i) {
    // Смещается только суша, и считается оно ОТ УРОВНЯ МОРЯ. Если смещать по абсолютной высоте, на
    // берегу выходит пила: соседние клетки различаются на четыре километра, и шельф торчит зубцами.
    // На глобусе вода и должна быть ровной сферой — глубину показывает цвет, а не форма.
    result[i].height = float(std::max(0.0, height.get(i) - sea_level));
    result[i].land = land.get(i) != 0.0 ? 1.0f : 0.0f;
  }

  // Клетка без метки: суша серо-бурая, вода тёмно-синяя. Пустое место на карте меток обязано
  // отличаться от воды, иначе «не занято» и «не земля» выглядят одинаково.
  const auto empty_colour = [&](const size_t index) {
    return land.get(index) != 0.0 ? glm::vec3(0.26f, 0.24f, 0.21f) : glm::vec3(0.05f, 0.08f, 0.15f);
  };

  // Непомеченная клетка получает РАЗНЫЕ номера областей на суше и на воде, хотя метки у неё нет ни
  // там, ни там. Иначе они попадают в одну область с двумя разными цветами, а заливка по покрытию
  // держится на том, что у области цвет один: берег между «незанятой землёй» и водой рисовался бы
  // границей КЛЕТКИ, то есть лестницей. Видно это на карте культур (занята не вся суша) и при
  // просмотре по шагам, где поле следующего шага ещё нулевое.
  const auto empty_area = [&](const size_t index) { return land.get(index) != 0.0 ? -1.0f : 0.0f; };

  const auto scalar = [&](const originator::const_field_accessor& field, const double low, const double high,
                          const std::span<const glm::vec3>& stops) {
    const double span = high - low;
    for (size_t i = 0; i < cells; ++i) {
      result[i].colour = ramp(span == 0.0 ? 0.0 : (field.get(i) - low) / span, stops);
    }
  };

  switch (mode) {
    case 0: { // климат
      const auto climate = field_of(source, "cells", "climate");
      for (size_t i = 0; i < cells; ++i) {
        const auto zone = size_t(climate.get(i));
        result[i].colour = climate_palette[std::min(zone, climate_palette.size() - 1)];
        result[i].area = float(zone + 1);
      }
      break;
    }
    case 1: { // рельеф
      for (size_t i = 0; i < cells; ++i) {
        result[i].colour = relief_colour(height.get(i), land.get(i) != 0.0);
      }
      break;
    }
    case 2: { // плиты
      const auto plate = field_of(source, "cells", "plate");
      const auto frontier = field_of(source, "cells", "convergent_seed");
      const auto divergent = field_of(source, "cells", "divergent_seed");
      for (size_t i = 0; i < cells; ++i) {
        glm::vec3 colour = label_colour(uint32_t(plate.get(i)));
        // Границы подсвечены поверх цвета плиты: у карты плит главный вопрос не «какая плита», а
        // «где стык и какого он рода».
        if (frontier.get(i) != 0.0) {
          colour = glm::mix(colour, glm::vec3(1.0f, 0.35f, 0.15f), 0.75f);
        } else if (divergent.get(i) != 0.0) {
          colour = glm::mix(colour, glm::vec3(0.25f, 0.55f, 1.0f), 0.75f);
        }
        result[i].colour = colour;
        result[i].area = float(plate.get(i) + 1.0);
      }
      break;
    }
    case 3: { // тектоника
      const auto convergence = field_of(source, "cells", "convergence");
      const auto subduction = field_of(source, "cells", "subduction");
      for (size_t i = 0; i < cells; ++i) {
        const double value = std::clamp(convergence.get(i) / 3.0, -1.0, 1.0);
        glm::vec3 colour = value >= 0.0
                             ? glm::mix(glm::vec3(0.16f, 0.17f, 0.19f), glm::vec3(0.95f, 0.25f, 0.15f), float(value))
                             : glm::mix(glm::vec3(0.16f, 0.17f, 0.19f), glm::vec3(0.20f, 0.55f, 0.95f), float(-value));
        if (subduction.get(i) != 0.0) {
          colour *= 0.45f;
        }
        result[i].colour = colour;
      }
      break;
    }
    case 4: { // температура
      const std::array<glm::vec3, 6> stops{{
        {0.62f, 0.74f, 0.92f}, {0.28f, 0.44f, 0.78f}, {0.30f, 0.68f, 0.60f},
        {0.86f, 0.82f, 0.36f}, {0.90f, 0.52f, 0.22f}, {0.72f, 0.14f, 0.12f}}};
      const auto summer = field_of(source, "cells", "temperature_summer");
      const auto winter = field_of(source, "cells", "temperature_winter");
      for (size_t i = 0; i < cells; ++i) {
        const double mean = 0.5 * (summer.get(i) + winter.get(i));
        result[i].colour = ramp((mean + 40.0) / 80.0, stops);
      }
      break;
    }
    case 5: { // сезонность
      const std::array<glm::vec3, 4> stops{{
        {0.10f, 0.12f, 0.16f}, {0.35f, 0.30f, 0.55f}, {0.85f, 0.55f, 0.25f}, {0.98f, 0.94f, 0.80f}}};
      scalar(field_of(source, "cells", "seasonality"), 0.0, 45.0, stops);
      break;
    }
    case 6: { // осадки
      const std::array<glm::vec3, 5> stops{{
        {0.72f, 0.62f, 0.42f}, {0.78f, 0.76f, 0.38f}, {0.42f, 0.66f, 0.32f},
        {0.16f, 0.52f, 0.42f}, {0.12f, 0.36f, 0.68f}}};
      scalar(field_of(source, "cells", "precipitation"), 0.0, 3.0, stops);
      break;
    }
    case 7: { // пригодность
      const std::array<glm::vec3, 4> stops{{
        {0.12f, 0.12f, 0.14f}, {0.36f, 0.32f, 0.20f}, {0.44f, 0.62f, 0.24f}, {0.86f, 0.95f, 0.55f}}};
      scalar(field_of(source, "cells", "habitability"), 0.0, 1.0, stops);
      break;
    }
    case 8: { // население
      const std::array<glm::vec3, 4> stops{{
        {0.08f, 0.09f, 0.12f}, {0.35f, 0.18f, 0.32f}, {0.86f, 0.48f, 0.22f}, {1.0f, 0.95f, 0.70f}}};
      const auto population = field_of(source, "cells", "population");
      // Логарифм, потому что население сосредоточено: линейная шкала показала бы несколько ярких
      // клеток и чёрную планету вокруг.
      for (size_t i = 0; i < cells; ++i) {
        const double value = std::log10(1.0 + std::max(0.0, population.get(i))) / 3.0;
        result[i].colour = ramp(value, stops);
      }
      break;
    }
    case 9: { // культуры
      const auto culture = field_of(source, "cells", "culture");
      for (size_t i = 0; i < cells; ++i) {
        // Земля без метки и вода без метки красятся РАЗНЫМ, и это не украшение: на первом кадре они
        // были одного тёмного цвета, и карта культур читалась как «культуры заняли пятнышко на пустой
        // планете». Замер показал обратное — занято 87% пригодной суши, — то есть врала отрисовка.
        result[i].colour = culture.get(i) != 0.0 ? label_colour(uint32_t(culture.get(i))) : empty_colour(i);
        result[i].area = culture.get(i) != 0.0 ? float(culture.get(i)) : empty_area(i);
      }
      break;
    }
    case 10: { // провинции
      const auto province = field_of(source, "cells", "province");
      for (size_t i = 0; i < cells; ++i) {
        result[i].colour = province.get(i) != 0.0 ? label_colour(uint32_t(province.get(i))) : empty_colour(i);
        result[i].area = province.get(i) != 0.0 ? float(province.get(i)) : empty_area(i);
      }
      break;
    }
    case 11: { // морские зоны
      const auto zone = field_of(source, "cells", "sea_zone");
      for (size_t i = 0; i < cells; ++i) {
        result[i].colour = zone.get(i) != 0.0 ? label_colour(uint32_t(zone.get(i))) : empty_colour(i);
        result[i].area = zone.get(i) != 0.0 ? float(zone.get(i)) : empty_area(i);
      }
      break;
    }
    case 12: { // области рельефа
      const auto kind = field_of(source, "cells", "landform");
      for (size_t i = 0; i < cells; ++i) {
        result[i].colour = landform_palette[std::min(size_t(kind.get(i)), landform_palette.size() - 1)];
        result[i].area = float(kind.get(i) + 1.0);
      }
      break;
    }
    default: { // уровни географической иерархии
      static constexpr std::array<const char*, 7> level_fields{
        {"land_mass", "continent", "historical_region", "ocean", "duchy", "empire", "realm"}};
      const auto level = field_of(source, "cells", level_fields[std::min<size_t>(mode - 13, 6)]);
      for (size_t i = 0; i < cells; ++i) {
        result[i].colour = level.get(i) != 0.0 ? label_colour(uint32_t(level.get(i))) : empty_colour(i);
        result[i].area = level.get(i) != 0.0 ? float(level.get(i)) : empty_area(i);
      }
      break;
    }
  }

  return result;
}

} // namespace devils_engine::gn02
