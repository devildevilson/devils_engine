#ifndef DEVILS_ENGINE_SIMUL_INTERPOLATION_H
#define DEVILS_ENGINE_SIMUL_INTERPOLATION_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>
#include <vector>

#include <gtl/phmap.hpp>

// Обобщённая интерполяция снапшотов для консьюмера (render thread). Три ортогональных концерна
// разнесены: (1) timing — «два снапшота + часы -> alpha», (2) identity — id -> индекс в prev,
// (3) blend — как смешать ОДИН инстанс (trait на тип). snapshot_interpolator связывает их и
// переиспользуется для ЛЮБОГО draw_intent-типа (актёры/тайлы/партиклы): валюта ввода/вывода —
// сырые байты draw_intent (bind() гарантирует stride==sizeof(Instance), т.е. плотный массив).

namespace devils_engine {
namespace simul {

// ── (2) identity: стабильный id -> индекс инстанса в ПРЕДЫДУЩЕМ снапшоте ──
template <typename Key = uint32_t>
class interpolation_track {
public:
  void rebuild(const std::span<const Key> previous_ids) {
    previous_index_.clear();
    previous_index_.reserve(previous_ids.size());
    for (uint32_t i = 0; i < previous_ids.size(); ++i) {
      previous_index_.emplace(previous_ids[i], i);
    }
  }

  uint32_t previous_index(const Key id, const uint32_t fallback) const noexcept {
    const auto itr = previous_index_.find(id);
    return itr == previous_index_.end() ? fallback : itr->second;
  }

private:
  gtl::flat_hash_map<Key, uint32_t> previous_index_;
};

// alpha из прошедшего времени и длительности интервала снапшота (оба в ОДНИХ единицах —
// utils::global_time_resolution). Клэмп [0,1]: экстраполяции нет, underflow замирает на cur.
float interpolation_alpha(size_t elapsed, size_t frame_time) noexcept;

// ── (1) timing: «два снапшота + часы -> alpha». nominal_clock копит РЕАЛЬНОЕ прошедшее
// время (корми advance() wall-clock дельтой рендер-кадра, НЕ номинальным шагом) — так джиттер
// планировщика и вариация частоты рендера не рассинхронят alpha (см. аудит, п.①). elapsed
// сбрасывается на приход снапшота; frame_time — длительность интервала до следующего снапшота.
// Остаточный дрейф от НЕноминальной каденции самого sim лечится таймстампами на снапшоте (option C).
struct nominal_clock {
  size_t elapsed = 0;
  size_t frame_time = 0;
  void on_snapshot(const size_t snap_frame_time) noexcept {
    elapsed = 0;
    frame_time = snap_frame_time;
  }
  void advance(const size_t real_elapsed) noexcept {
    elapsed += real_elapsed;
  }
  float alpha() const noexcept {
    return interpolation_alpha(elapsed, frame_time);
  }
};

// ── (3) blend: как смешать ОДИН инстанс. Первичный шаблон намеренно БЕЗ определения — забыл
// специализировать тип => ошибка компиляции, а не молчаливый снап всех полей. Специализация
// живёт рядом с потребителем (см. render_system.cpp: blend_traits<actor_instance>). Соглашение:
// continuous-поля (позиция/размер/угол) лерпятся, discrete (текстура/индексы) снапаются к
// новейшему (b). mix МОЖЕТ реализовать teleport-guard: при слишком большой Δ вернуть b без лерпа.
template <typename T>
struct blend_traits;

// Обобщённый интерполятор снапшотов: держит prev/cur ТИПИЗИРОВАННО, матчит по стабильному id,
// смешивает через blend_traits<Instance>. Ввод/вывод — сырые байты draw_intent (плотный массив
// Instance), поэтому один класс переиспользуется для любого draw_intent-типа.
template <typename Instance, typename Key = uint32_t, typename Clock = nominal_clock>
class snapshot_interpolator {
  static_assert(std::is_trivially_copyable_v<Instance>, "Instance must be trivially copyable (decode/encode == memcpy)");

public:
  // Принять новый снапшот: cur -> prev, декодировать байты в cur, переиндексировать prev.
  // snap_frame_time — длительность до СЛЕДУЮЩЕГО снапшота (единицы global_time_resolution).
  void push(const std::span<const uint8_t> bytes, const std::span<const Key> ids, const size_t snap_frame_time) {
    prev_ = std::move(cur_);
    have_prev_ = have_cur_;
    decode(bytes, ids, cur_);
    have_cur_ = true;
    track_.rebuild(std::span<const Key>(prev_.ids));
    clock_.on_snapshot(snap_frame_time);
  }

  // Продвинуть часы на РЕАЛЬНО прошедшее время рендер-кадра (единицы global_time_resolution).
  void advance(const size_t real_elapsed) noexcept {
    clock_.advance(real_elapsed);
  }

  bool has_data() const noexcept {
    return have_cur_;
  }
  uint32_t count() const noexcept {
    return uint32_t(cur_.instances.size());
  }
  float alpha() const noexcept {
    return clock_.alpha();
  }

  // Записать интерполированные инстансы текущего снапшота в out (байты, плотный Instance).
  // Нет prev -> сырой cur (fallback). Возврат — число инстансов.
  uint32_t resolve(std::vector<uint8_t>& out) const {
    const uint32_t n = uint32_t(cur_.instances.size());
    out.resize(size_t(n) * sizeof(Instance));
    if (n == 0) {
      return 0;
    }
    std::memcpy(out.data(), cur_.instances.data(), size_t(n) * sizeof(Instance));
    if (!have_prev_) {
      return n; // первый снапшот: интерполировать не с чем
    }

    const float t = clock_.alpha();
    auto* const dst = reinterpret_cast<Instance*>(out.data());
    for (uint32_t i = 0; i < n; ++i) {
      const uint32_t pi = track_.previous_index(cur_.ids[i], UINT32_MAX);
      if (pi < prev_.instances.size()) {
        dst[i] = blend_traits<Instance>::mix(prev_.instances[pi], cur_.instances[i], t);
      }
    }
    return n;
  }

private:
  struct frame {
    std::vector<Instance> instances;
    std::vector<Key> ids;
  };

  static void decode(const std::span<const uint8_t> bytes, const std::span<const Key> ids, frame& out) {
    const size_t by_bytes = bytes.size() / sizeof(Instance);
    const size_t n = std::min(ids.size(), by_bytes);
    out.instances.resize(n);
    if (n != 0) {
      std::memcpy(out.instances.data(), bytes.data(), n * sizeof(Instance));
    }
    out.ids.assign(ids.begin(), ids.begin() + n);
  }

  frame prev_, cur_;
  interpolation_track<Key> track_;
  Clock clock_;
  bool have_prev_ = false;
  bool have_cur_ = false;
};

} // namespace simul
} // namespace devils_engine

#endif
