#ifndef TILE_FRONTIER_CORE_SIMULATION_H
#define TILE_FRONTIER_CORE_SIMULATION_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <utility>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/simul/game_host.h>
#include <devils_engine/simul/standard_sound_system.h>

namespace tile_frontier {
namespace core {

struct simulation_init;
struct broker;
struct runtime_bootstrap;
struct runtime_traits;
class render_simulation;
class assets_simulation;

using sound_simulation = devils_engine::simul::standard_sound_system<broker>;

// Главная/gameplay система. Движковый lifecycle/loop-скелет живёт в simul::game_host; проект
// реализует набор хуков (проектная сцена/актёры/биндинги/публикация кадра) и владеет состоянием.
//
// main_system/advancer виртуали — ТОНКИЕ out-of-line форвардеры в host_* хелперы game_host (init →
// host_init и т.д.). Так vtable и инстанцирование трогающих state методов остаются в этом .cpp, где
// simulation_init полный (иначе inline-виртуали шаблона инстанцировались бы в TU пользователя при
// forward-declared состоянии).
class simulation : public devils_engine::simul::game_host<simulation, runtime_bootstrap, broker> {
public:
  explicit simulation(runtime_bootstrap* boot = nullptr) noexcept;
  ~simulation() noexcept;

  void init() override;
  bool stop_predicate() const override;
  void update(size_t time) override;
  void workers_started() override;
  void runtime_settings_reloaded() override;

  simulation_init& state();
  const simulation_init& state() const;

  // ── проектные хуки game_host (дёргаются движковым host через derived(); публичны, чтобы не
  //    завязываться на friend — внутри тела класса имя `broker` затеняется методом брокера) ──
  void project_init();                                        // аллоцирует состояние + резолвит подсистемы + календарь
  devils_engine::demiurg::resource_system* asset_registry();  // реестр ассетов (шрифты/сцена/скрипты)
  void begin_project_loading();                               // проектная сцена (текстуры/звуки/чанки/актёры)
  bool project_loading_complete() const;                      // все mock-чанки применены
  std::pair<std::size_t, std::size_t> project_loading_progress() const; // вклад чанков {done,total} в прогресс
  void start_project_ui();                                    // визаж + lua-биндинги + entry (движковый split — шаг 3)
  void on_framebuffer_resize(uint32_t w, uint32_t h);         // cam.aspect от живого размера
  void update_gameplay(size_t time, uint64_t game_dt_ticks,
                       const devils_engine::simul::phase_gate& gate); // середина кадра (тайлы/актёры/звук/метрики)
  void on_visage_before_update();                             // tf_* env + слияние sound_state перед visage
  void project_settings_reloaded();                           // проектная реакция на reload настроек

private:
  std::unique_ptr<simulation_init> container;
};

} // namespace core
} // namespace tile_frontier

#endif
