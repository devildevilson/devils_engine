#ifndef DEVILS_ENGINE_PF09_CLIPMAP_H
#define DEVILS_ENGINE_PF09_CLIPMAP_H

// Клипмап идентификаторов территорий: пирамида уровней, каждый из которых покрывает вдвое больше мира
// вдвое более крупным текселем и всегда центрирован на наблюдателе.
//
// Это виртуальная текстура, у которой таблица страниц не хранится, а вычисляется. Отсюда всё остальное:
// нет readback-латентности, нет политики вытеснения внутри уровня, а адрес текселя в текстуре — это его
// мировой индекс по модулю стороны. Последнее и делает обновление дешёвым: когда окно съезжает, тексели,
// вышедшие за край, физически лежат ровно там, куда должны лечь новые. Обновляются две L-образные полосы,
// а не весь уровень.
//
// Уровень хранит НЕ листовой ярус, а самый глубокий из разрешимых на его масштабе: на тексель в километр
// участок не ложится, и записывать его туда значило бы записывать шум. Более крупные ярусы из уровня не
// хранятся вовсе — они получаются подъёмом по дереву от того, что записано. Это и есть zone LOD: карта
// хранит один ярус на уровень, а показать умеет любой ярус не глубже него.
//
// Модуль намеренно не знает про Vulkan. Он решает, ЧТО лежит в текселе и КАКИЕ прямоугольники надо залить;
// чем именно их заливать — вопрос среза 2b.

#include <cstdint>
#include <span>
#include <vector>

#include <glm/ext/vector_int2_sized.hpp>
#include <glm/vec2.hpp>

#include "territory.h"

namespace devils_engine::pf09 {

struct clipmap_config {
  uint32_t side = 512;             // тексели на сторону уровня
  uint32_t resident_levels = 6;    // сколько слотов в пуле
  // Два РАЗНЫХ вопроса, которые нельзя держать одним числом. Первый: насколько мелок самый мелкий
  // тексель — это про резкость картинки вблизи. Второй: сколько текселей должна занимать ячейка, чтобы
  // считаться разрешимой на уровне — это про то, какой ярус уровень хранит. Пока это был один параметр,
  // настройка под резкость делала карту вырожденной, а настройка под ярус — блочной.
  double base_texel_m = 0.0;       // ноль означает «вывести из размера листовой территории»
  uint32_t min_tier_texels = 8;    // сколько текселей занимает ячейка яруса, чтобы считаться разрешимой
  double hysteresis_octaves = 0.3; // запас при переключении окна резидентности, в долях уровня

  // Сколько уровней держать сверх нужных с каждой стороны. Юбка существует ровно затем, чтобы уровень
  // пёкся ДО того, как его понадобится показать: зум проходит октаву за сотни миллисекунд, и этого
  // времени достаточно, если начать заранее.
  uint32_t prefetch_levels = 1;

  // Потолок полной печки за одно наведение камеры, в текселях. Ноль снимает ограничение. Полосы
  // панорамирования в бюджет НЕ входят: валидный уровень обязан остаться валидным, иначе окно поедет, а
  // содержимое отстанет, и это будет уже не «менее детально», а просто неверно.
  uint64_t bake_budget_texels = 0;
};

// Прямоугольник заливки в текселях текстуры уровня. Границы уже развёрнуты по модулю, поэтому регион
// никогда не пересекает шов и ложится в один `vkCmdCopyBufferToImage`.
struct upload_region {
  uint32_t level = 0;
  uint32_t x = 0;
  uint32_t y = 0;
  uint32_t width = 0;
  uint32_t height = 0;

  uint64_t texel_count() const noexcept { return uint64_t(width) * height; }
};

// Что стоило последнее наведение камеры. Считается в текселях и в регионах отдельно: тексели — это
// трафик шины, регионы — это число команд копирования, и упереться можно в любое из двух.
struct update_cost {
  uint64_t texels = 0;
  uint32_t regions = 0;
  uint32_t rebuilt_levels = 0;  // уровни, ЗАВЕРШИВШИЕ полную печку на этом наведении
  uint32_t shifted_levels = 0;  // уровни, обновлённые полосами
  uint32_t pending_levels = 0;  // уровни, печка которых не уместилась в бюджет и продолжится
  uint32_t starved_levels = 0;  // нужные МЕЛКИЕ уровни, которым не хватило слотов пула
};

// Какие два уровня смешивать в точке и с каким весом. Вес принадлежит вызывающему, а не сэмплеру: у
// уровней разный масштаб и разное тороидальное смещение, поэтому у каждого своя `uv`, и аппаратная
// интерполяция поперёк уровней смешала бы правильные значения по неправильным координатам.
struct level_pick {
  uint32_t fine = 0;
  uint32_t coarse = 0;
  double weight = 0.0;  // 0 — целиком мелкий уровень, 1 — целиком крупный
  bool covered = false; // нашёлся ли вообще хоть один готовый уровень, накрывающий точку
};

class clipmap {
public:
  clipmap(const territory& map, const clipmap_config& config);

  const clipmap_config& config() const noexcept { return config_; }

  uint32_t level_count() const noexcept { return level_count_; }
  double texel_size_m(const uint32_t level) const noexcept;
  double coverage_m(const uint32_t level) const noexcept;
  tier level_tier(const uint32_t level) const noexcept { return level_tier_[level]; }

  // Уровень готов, только если его содержимое целиком соответствует текущему окну. Печущийся уровень
  // намеренно не отдаёт тексели: показать половину испечённого окна хуже, чем показать грубый уровень.
  bool ready(const uint32_t level) const noexcept { return resident(level) && levels_[level].valid; }

  uint32_t first_resident() const noexcept { return first_resident_; }
  uint32_t resident_count() const noexcept { return resident_count_; }
  bool resident(const uint32_t level) const noexcept {
    return level >= first_resident_ && level < first_resident_ + resident_count_;
  }

  uint64_t resident_bytes() const noexcept;

  // Самый мелкий уровень, тексель которого не меньше метров на пиксель, и самый крупный, покрытия
  // которого хватает на дальность видимости. Разность этих двух — сколько уровней зум ТРЕБУЕТ; она может
  // оказаться больше пула, и тогда об этом надо сказать, а не молча показать дыру.
  uint32_t required_first(const double meters_per_pixel) const noexcept;
  uint32_t required_last(const double view_distance_m) const noexcept;

  // Навести камеру. Возвращает стоимость обновления; регионы заливки лежат в `regions()`.
  update_cost focus(const glm::dvec2& center_m, const double meters_per_pixel, const double view_distance_m);

  std::span<const upload_region> regions() const noexcept { return regions_; }

  // Чтение так, как его сделает шейдер: мировая точка -> тексель -> модуль стороны -> выборка.
  // `invalid_zone`, если уровень не резидентен или точка вне его окна.
  zone_id sample(const glm::dvec2& point_m, const uint32_t level) const;

  // Пара уровней для плавного перехода. Мелкий и крупный выбираются из ГОТОВЫХ и накрывающих точку,
  // поэтому во время печки пара просто съезжает вверх, а картинка теряет резкость, но не пропадает.
  level_pick pick(const glm::dvec2& point_m, const double meters_per_pixel) const;

  // Самый мелкий готовый уровень, накрывающий точку. `level_count()`, если такого нет — это дыра, и
  // именно её ищет проверка непрерывного зума.
  uint32_t serving_level(const glm::dvec2& point_m) const;

  // Мировой центр текселя. Публичен, потому что проверка «тексель равен прямому resolve» обязана щупать
  // ровно ту точку, по которой тексель и пекли.
  glm::dvec2 texel_center_m(const glm::i64vec2 texel, const uint32_t level) const;
  glm::i64vec2 window_origin(const uint32_t level) const noexcept { return levels_[level].origin; }

  // Полная перепечка уровня без учёта инкрементальности. Нужна проверке: инкрементальный результат обязан
  // совпадать с ней побайтно.
  std::vector<zone_id> bake_reference(const uint32_t level, const glm::dvec2& center_m) const;

private:
  struct level_state {
    glm::i64vec2 origin{};      // мировой индекс текселя левого нижнего угла окна
    bool valid = false;         // содержимое целиком соответствует `origin`
    bool baking = false;        // полная печка начата и не закончена
    int64_t baked_rows = 0;     // сколько строк окна уже испечено
    std::vector<zone_id> texels;
  };

  glm::i64vec2 origin_for(const glm::dvec2& center_m, const uint32_t level) const;
  bool contains(const glm::dvec2& point_m, const uint32_t level) const;
  void advance_bake(const uint32_t level, uint64_t& budget, update_cost& cost);
  void fill_rect(const uint32_t level, const glm::i64vec2 begin, const glm::i64vec2 end, update_cost& cost);
  void emit_region(const uint32_t level, const glm::i64vec2 begin, const glm::i64vec2 end, update_cost& cost);

  const territory* map_ = nullptr;
  clipmap_config config_;
  uint32_t level_count_ = 0;
  double base_texel_m_ = 0.0;

  std::vector<double> texel_size_;
  std::vector<tier> level_tier_;
  std::vector<level_state> levels_;
  std::vector<upload_region> regions_;

  uint32_t first_resident_ = 0;
  uint32_t needed_first_ = 0;
  uint32_t resident_count_ = 0;
  bool focused_ = false;
};

} // namespace devils_engine::pf09

#endif
