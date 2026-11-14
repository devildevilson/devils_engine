#ifndef FRONTIER_ONLINE_NET_LIVE_RUNTIME_H
#define FRONTIER_ONLINE_NET_LIVE_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/authority.h"
#include "net/follower.h"

// ЖИВАЯ СЕССИЯ: авторитет тикает, ведомые следуют за каноническими bundles.
//
// Классы ниже — сессия без часов и без цикла: каждый круг вызывает владелец (процессный драйвер
// `run_live_*` или тест в одном процессе), поэтому порядок событий в тесте управляем, а не
// зависит от планировщика ОС. `net_runtime` тоже чужой: два конца в одном процессе обязаны
// делить один диспетчер GNS.

namespace frontier_online::core {
class causal_content;
}

namespace frontier_online::net {

struct live_run_options {
  uint16_t port = 0;
  bool bind_loopback = false;
  std::string endpoint;
  uint32_t expected_peers = 2;
  uint32_t actors = 128;
  uint32_t workers = 2;
  uint32_t rate = 60;
  uint32_t timeout_seconds = 60;
  uint64_t final_tick = 180;
  uint64_t world_seed = 20260911;
  std::string generator = "generator/world";
  std::vector<std::string> prefab_cycle{"prey", "prey", "prey", "actor"};

  // Бюджет удерживаемой истории canonical bundles: и число, и байты. Отставший за её начало
  // участник отключается с причиной; остальные продолжают.
  size_t history_ticks = 512;
  size_t history_bytes = 512 * 1024;
  // Сколько записей ОДИН участник может занять в одном тике. Общий предел тика выводится из
  // формата canonical_tick (число записей и байты), см. `live_tick_record_capacity`.
  uint32_t records_per_peer_per_tick = 2;
  // Окно будущих тиков, на которые авторитет принимает предложения.
  uint32_t intent_window_ticks = 32;
  // Ведомый: один нарочно неверный локальный прогноз, чтобы путь исправления был наблюдаемым.
  bool predict_wrong = true;
};

// Сколько записей `spawn_food` помещается в один canonical tick с корнем: меньшее из предела
// записей формата и того, что влезает в предел байт.
[[nodiscard]] uint32_t live_tick_record_capacity() noexcept;

class live_authority {
public:
  struct counters {
    uint32_t applied_inputs = 0;
    uint64_t dropped_inputs = 0;
    uint64_t duplicate_inputs = 0; // повторы уже принятых предложений: штатная избыточность
    uint64_t canonical_bytes_sent = 0;
  };

  live_authority(net_runtime& runtime, const core::causal_content& content,
                 const live_run_options& options, const authority_config& config);
  ~live_authority();
  live_authority(const live_authority&) = delete;
  live_authority& operator=(const live_authority&) = delete;

  // Сетевой круг: рукопожатия, приём предложений, доставка bundles. Тик не делает.
  void service();
  // Один причинный тик: журнал принятых предложений → шаг → canonical bundle → история.
  void advance();

  [[nodiscard]] uint16_t port() const noexcept;
  [[nodiscard]] uint64_t tick() const noexcept;
  [[nodiscard]] uint64_t root() const;
  [[nodiscard]] const counters& stats() const noexcept;
  // Каждому живому участнику отправлены bundles по `tick` включительно.
  [[nodiscard]] bool delivered_through(uint64_t tick) const;
  [[nodiscard]] authority& endpoint() noexcept;
  [[nodiscard]] const authority& endpoint() const noexcept;

private:
  struct state;
  std::unique_ptr<state> state_;
};

class live_follower {
public:
  struct counters {
    uint64_t checkpoint_tick = 0;
    bool corrected = false;
    uint32_t own_inputs = 0;      // свои предложения, найденные в канонических bundles
    uint32_t rejected_inputs = 0; // свои предложения, чей тик прошёл без записи
    uint64_t bundles_verified = 0;
    uint64_t canonical_bytes = 0;
  };

  live_follower(net_runtime& runtime, const core::causal_content& content,
                const follower_config& config, const live_run_options& options);
  ~live_follower();
  live_follower(const live_follower&) = delete;
  live_follower& operator=(const live_follower&) = delete;

  [[nodiscard]] bool start();
  // Шаг ведомого: приём, затем исполнение всех полученных bundles со сверкой корня на каждом.
  // Расхождение корня — критическая ошибка (std::runtime_error), а не исход.
  void service();
  // Предложить `spawn_food(x, y)` на тик `target`. Только после подтверждения присоединения и
  // пока в полёте меньше 16 неподтверждённых. Повторы до подтверждения — забота ведомого.
  [[nodiscard]] bool propose_food(double x, double y, uint64_t target);

  [[nodiscard]] bool ready() const noexcept;     // присоединился, подтверждён, состояние загружено
  [[nodiscard]] bool caught_up() const noexcept; // получил live_caught_up и дошёл до него
  [[nodiscard]] uint64_t tick() const noexcept;
  [[nodiscard]] uint64_t root() const;
  [[nodiscard]] const counters& stats() const noexcept;
  [[nodiscard]] follower& endpoint() noexcept;
  [[nodiscard]] const follower& endpoint() const noexcept;

private:
  struct state;
  std::unique_ptr<state> state_;
};

int run_live_authority(const live_run_options& options, const core::causal_content& content);
int run_live_follower(const live_run_options& options, const core::causal_content& content);

} // namespace frontier_online::net

#endif
