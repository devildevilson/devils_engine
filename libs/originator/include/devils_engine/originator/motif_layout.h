#ifndef DEVILS_ENGINE_ORIGINATOR_MOTIF_LAYOUT_H
#define DEVILS_ENGINE_ORIGINATOR_MOTIF_LAYOUT_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

// Автор задаёт БЛОКИ и программу их пристыковки; сборщик выбирает размеры и положения. Здесь нет
// понятий комнаты и коридора: `motif` — id ресурса/процедуры проекта, `location` — id осмысленного
// места. Окружение передаётся неизменяемой маской обнажённой поверхности в тех же единицах плана.
// Результат — список в мере. Картинка и ресурсный жизненный цикл принадлежат потребителю.

namespace devils_engine::originator {

inline constexpr uint32_t no_motif_parent = UINT32_MAX;

enum motif_side : uint8_t {
  motif_east = 1u << 0,
  motif_west = 1u << 1,
  motif_south = 1u << 2,
  motif_north = 1u << 3,
};

enum class motif_attach_align : uint8_t { random, start, centre, end };

struct motif_rect {
  int32_t x = 0;
  int32_t y = 0;
  int32_t w = 0;
  int32_t h = 0;

  bool operator==(const motif_rect&) const noexcept = default;
};

struct motif_rule {
  uint32_t motif = 0;
  uint32_t location = 0;
  uint32_t parent = no_motif_parent; // индекс ПРАВИЛА, меньше собственного индекса
  uint32_t min_width = 0;
  uint32_t max_width = 0;
  uint32_t min_height = 0;
  uint32_t max_height = 0;
  uint8_t sides = 0;               // стороны родителя, у которых можно поставить этот блок
  uint8_t chance_percent = 100;    // необязательные блоки — только листья программы
  motif_attach_align align = motif_attach_align::random; // участок кромки родителя, а не сторона
  motif_attach_align passage_align = motif_attach_align::random; // место прохода на общем участке
  bool strict_align = false; // не перебирать другие смещения, если этот участок обязателен
  bool direct_join = false; // общая кромка с родителем без клетки-прохода
  uint32_t passage_inset = 0; // отступ точки прохода от концов общего участка
};

struct motif_instance {
  motif_rect rect{};
  uint32_t motif = 0;
  uint32_t location = 0;
  uint32_t rule = 0;
  uint32_t parent = no_motif_parent; // индекс ЭКЗЕМПЛЯРА
  bool direct_join = false;

  bool operator==(const motif_instance&) const noexcept = default;
};

struct motif_passage {
  uint32_t a = no_motif_parent; // внешний вход: a == no_motif_parent
  uint32_t b = 0;
  int32_t x = 0;               // одна мера стены, открытая проходом
  int32_t y = 0;

  bool operator==(const motif_passage&) const noexcept = default;
};

struct motif_exposure {
  uint32_t instance = 0;
  int32_t x = 0;
  int32_t y = 0;

  bool operator==(const motif_exposure&) const noexcept = default;
};

struct motif_layout {
  std::vector<motif_instance> instances;
  std::vector<motif_passage> passages;
  std::vector<motif_exposure> exposures; // каждое пересечение пола с поверхностью видно потребителю
  std::string refusal;
  int32_t width = 0;
  int32_t height = 0;
  int32_t entry_y = 0;           // внешний порт (0, entry_y)
  uint32_t attempts = 0;

  bool valid() const noexcept { return refusal.empty(); }
};

// Неизменная маска `surface_exposure`: 1 значит, что поверхность обнажила потолок в этой мере.
// Пустая маска означает отсутствие известного пересечения; непустая имеет ровно width*height байт.
// Отказ с причиной — обычный исход для программы, не вмещающейся в заявленное пятно.
motif_layout assemble_motifs(std::span<const motif_rule> rules, int32_t width, int32_t height,
                             int32_t entry_y, uint64_t seed,
                             std::span<const uint8_t> surface_exposure = {}, uint32_t max_attempts = 64);

// Независимая проверка уже построенного плана: геометрия проходов, обход всех экземпляров от
// внешнего входа и возвращение к нему. Пустая строка значит, что все обещания выдержаны.
std::string validate_motif_layout(const motif_layout& layout);

} // namespace devils_engine::originator

#endif
