#ifndef TILE_FRONTIER_CORE_DRAW_INTENT_H
#define TILE_FRONTIER_CORE_DRAW_INTENT_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string_view>

#include "instance_layout.h"

// Главная сторона контракта main -> render: типизированный упаковщик инстансов в СЫРЫЕ БАЙТЫ.
// Даём draw_intent C++-структуру T, он пакует её в байты формата instance_layout draw_group
// ("v4", "v3c4", "v4ui1", ...). T привязывается к layout один раз (bind()), матчер проверяет
// соответствие ПО ТОКЕНАМ + по GPU-страйду. Раз совпало — упаковка это memcpy: для разрешённых
// instance_layout типов (float/uint32/int32/glm::vec/std::array из них) все листья >=4 байт и
// кратны 4, значит C++- и GPU-раскладка байт-в-байт идентичны и GPU-паддинг вырождается в ноль.
// Если когда-нибудь появится суб-4-байтный формат или padding-дыра в T — bind() вернёт ошибку
// ДО любой записи, а не молча уедет битыми байтами на GPU.
//
// ZERO-ALLOC: draw_intent НЕ владеет буфером. Хранилище байт (маппнутая GPU-память либо буфер под
// сообщение) всегда даёт вызывающий — это держит hot-path свободным от аллокаций.

namespace tile_frontier {
namespace core {

template <typename T>
class draw_intent {
  static_assert(std::is_trivially_copyable_v<T>, "instance type must be trivially copyable (packing == memcpy)");
  static_assert(instance_layout::count_atoms<T>() > 0, "T must decompose into at least one attribute");

public:
  draw_intent() = default;

  // привязка к layout-строке draw_group ("v4", "v2ui1", ...)
  instance_layout::match_result bind(const std::string_view& layout) {
    match_ = instance_layout::check<T>(layout);
    return match_;
  }

  // привязка к УЖЕ РАСПАРСЕННОМУ layout draw_group: атомы (draw_group::instance_layout) + страйд
  // (draw_group::stride). Так хедер не зависит от painter/structures.h — render-сторона, у которой
  // полный draw_group, передаёт поля явно. format::values приходит из лёгкого painter/common.h.
  instance_layout::match_result bind(const std::span<const devils_engine::painter::format::values>& layout, const uint32_t stride) {
    match_ = instance_layout::check<T>(layout, stride);
    return match_;
  }

  bool valid() const noexcept { return match_.ok(); }
  const instance_layout::match_result& result() const noexcept { return match_; }
  static constexpr uint32_t stride() noexcept { return uint32_t(sizeof(T)); }

  // упаковать инстансы прямо в маппнутую GPU-память (instance frame) по байтовому оффсету.
  // вызывающий гарантирует ёмкость (max_count * stride). Возврат — сколько байт записано.
  std::size_t blit(const std::span<const T>& src, void* mapped, const std::size_t byte_offset) const {
    const std::size_t bytes = src.size_bytes();
    if (bytes != 0) std::memcpy(static_cast<uint8_t*>(mapped) + byte_offset, src.data(), bytes);
    return bytes;
  }

  // упаковать инстансы в буфер вызывающего (напр. payload сообщения main->render).
  // пишет min(src, dst) байт, чтобы не вылезти за dst. Возврат — сколько байт записано.
  std::size_t blit(const std::span<const T>& src, const std::span<uint8_t>& dst) const {
    const std::size_t bytes = std::min(src.size_bytes(), dst.size());
    if (bytes != 0) std::memcpy(dst.data(), src.data(), bytes);
    return bytes;
  }

private:
  instance_layout::match_result match_{};
};

} // namespace core
} // namespace tile_frontier

#endif
