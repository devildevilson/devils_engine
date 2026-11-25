#ifndef FRONTIER_ONLINE_NET_LIVE_RUNTIME_H
#define FRONTIER_ONLINE_NET_LIVE_RUNTIME_H

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "net/authority.h"
#include "net/follower.h"
#include "net/network_settings.h"

// ЖИВАЯ СЕССИЯ: авторитет тикает, ведомые следуют за каноническими bundles.
//
// Классы ниже — сессия без часов и без цикла: каждый круг вызывает владелец (процессный драйвер
// `run_live_*` или тест в одном процессе), поэтому порядок событий в тесте управляем, а не
// зависит от планировщика ОС. `net_runtime` тоже чужой: два конца в одном процессе обязаны
// делить один диспетчер GNS.

namespace devils_engine::thread {
class atomic_pool;
}

namespace frontier_online::core {
class actor_world_slice;
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
  uint32_t chunk_size = 16;
  // Мировой бокс [0.5, extent]^2. Едет в checkpoint: клиент строит по нему сетку и камеру.
  float extent = 64.0f;
  std::vector<std::string> prefab_cycle{"prey", "prey", "prey", "actor"};

  // Бюджет удерживаемой истории canonical bundles: и число, и байты. Отставший за её начало
  // участник отключается с причиной; остальные продолжают.
  size_t history_ticks = 512;
  size_t history_bytes = 512 * 1024;
  // Кольцо снимков для догона вернувшегося участника, выпавшего из истории: снимок K и тики
  // K+1..N из истории. Снимок старше начала истории бесполезен, поэтому шаг — доля истории, а
  // кольцу хватает двух снимков. 0 — четверть истории.
  uint32_t checkpoint_every_ticks = 0;
  size_t checkpoint_ring_count = 2;
  size_t checkpoint_ring_bytes = 32 * 1024 * 1024;
  // Сколько записей ОДИН участник может занять в одном тике. Общий предел тика выводится из
  // формата canonical_tick (число записей и байты), см. `live_tick_record_capacity`.
  uint32_t records_per_peer_per_tick = 2;
  // Окно будущих тиков, на которые авторитет принимает предложения.
  uint32_t intent_window_ticks = 32;
  // Ведомый: один нарочно неверный локальный прогноз, чтобы путь исправления был наблюдаемым.
  bool predict_wrong = true;
  // Авторитет: процессный сценарий — ровно два ведомых, и каждый присылает ровно одно действие
  // (так их проверяет итоговая сверка). false — сессия для людей (`--play`): участников от
  // одного, действий сколько сделают, опоздавший клик — штатный отказ, а не провал прогона.
  bool scripted_inputs = true;
  // События рукопожатия обеих сторон (`--verbose`): где оборвался вход, когда процесс снят снаружи.
  bool verbose = false;
  // Сетевые сроки процессных драйверов (run_live_*); сервер читает их из `network.tavl`.
  network_settings network;
};

// Сколько записей `spawn_food` помещается в один canonical tick с корнем: меньшее из предела
// записей формата и того, что влезает в предел байт.
[[nodiscard]] uint32_t live_tick_record_capacity() noexcept;

class live_authority {
public:
  struct counters {
    uint32_t applied_inputs = 0;  // действия участников (без записей присоединения)
    uint32_t admitted_peers = 0;  // канонические присоединения: столько игроков появилось в мире
    uint32_t departed_peers = 0;  // канонические уходы: столько игроков исчезло из мира
    uint64_t dropped_inputs = 0;
    uint64_t duplicate_inputs = 0; // повторы уже принятых предложений: штатная избыточность
    uint64_t canonical_bytes_sent = 0;
    uint32_t journal_resumes = 0;    // продолжения с якоря клиента: только тики из истории
    uint32_t checkpoint_resumes = 0; // продолжения со снимка кольца
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

// Исход живого ведомого по данным авторитета. Это не ошибки процесса: что делать дальше, решает
// владелец (графический клиент входит заново со свежим checkpoint, процессный прогон — провал).
enum class live_failure : uint8_t {
  none,
  malformed_bundle, // bundle не разбирается или нарушает порядок тиков: авторитет нарушил протокол
  diverged,         // запись не применяется к локальному миру или корень после тика другой
};
[[nodiscard]] const char* describe(live_failure value) noexcept;

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

  // pool — пул владельца (графический клиент: тот же, что у его кадра); nullptr — свой пул на
  // options.workers. Пулом пользуется только поток, вызывающий service().
  live_follower(net_runtime& runtime, const core::causal_content& content,
                const follower_config& config, const live_run_options& options,
                devils_engine::thread::atomic_pool* pool = nullptr);
  ~live_follower();
  live_follower(const live_follower&) = delete;
  live_follower& operator=(const live_follower&) = delete;

  [[nodiscard]] bool start();
  // Шаг ведомого: приём, затем исполнение всех полученных bundles со сверкой корня на каждом.
  // Расхождение и неразбираемый bundle — исход `failure()`; после него ведомый больше не шагает.
  void service();
  [[nodiscard]] live_failure failure() const noexcept;
  // Предложить `spawn_food(x, y)` на тик `target`. Только после admitted() и
  // пока в полёте меньше 16 неподтверждённых. Повторы до подтверждения — забота ведомого.
  [[nodiscard]] bool propose_food(double x, double y, uint64_t target);

  // Повторный вход с продолжением: новое соединение с ключом последнего `reconnect_grant` и
  // подтверждённым состоянием (тик, корень). Мир, неподтверждённые предложения и номера ввода
  // остаются. false — продолжать нечем (нет ключа, не входил, авторитет нарушил протокол) или
  // транспорт не открыл соединение.
  [[nodiscard]] bool resume();
  [[nodiscard]] bool can_resume() const noexcept;
  [[nodiscard]] bool resuming() const noexcept; // продолжение идёт, ещё не подтверждено
  [[nodiscard]] uint32_t hold_ms() const noexcept; // окно удержания из ключа; 0 — ключа нет
  [[nodiscard]] uint32_t resumes() const noexcept; // согласованные продолжения
  // Объявление мира первого входа: продолжение мира не присылает.
  [[nodiscard]] const world_declaration& declared_world() const noexcept;

  [[nodiscard]] bool ready() const noexcept;     // присоединился, подтверждён, состояние загружено
  // Исполнена своя каноническая запись присоединения: в мире есть свой игрок. Раньше предложения
  // не принимаются — авторитет отбросил бы их как адресованные несуществующему игроку.
  [[nodiscard]] bool admitted() const noexcept;
  [[nodiscard]] bool caught_up() const noexcept; // получил live_caught_up и дошёл до него
  [[nodiscard]] uint64_t tick() const noexcept;
  [[nodiscard]] uint64_t root() const;
  // Мир последнего проверенного тика — то, что показывает графический клиент. Меняется только
  // внутри service().
  [[nodiscard]] const core::actor_world_slice& world() const noexcept;
  [[nodiscard]] const counters& stats() const noexcept;
  [[nodiscard]] follower& endpoint() noexcept;
  [[nodiscard]] const follower& endpoint() const noexcept;

private:
  struct state;
  std::unique_ptr<state> state_;
};

int run_live_authority(const live_run_options& options, const core::causal_content& content);
// Этап запуска живого процесса в stderr с миллисекундами от первого вызова (только при `on`):
// если процесс сняли снаружи, последний напечатанный этап говорит, где он стоял.
void trace_live_phase(bool on, const char* name);
int run_live_follower(const live_run_options& options, const core::causal_content& content);

} // namespace frontier_online::net

#endif
