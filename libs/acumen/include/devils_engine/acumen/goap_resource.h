#ifndef DEVILS_ENGINE_ACUMEN_GOAP_RESOURCE_H
#define DEVILS_ENGINE_ACUMEN_GOAP_RESOURCE_H

#include <string>
#include <vector>

#include <devils_engine/act/script_compiler.h>
#include <devils_engine/demiurg/resource_base.h>
#include <devils_script/container.h>
namespace devils_engine {
namespace demiurg {
class resource_system;
}
} // namespace devils_engine

// Generic disk config for acumen. It is parsed as a TAVL event stream because metric/effect rows
// contain embedded devils_script expressions consumed by the injected act::script_compiler.
//
//   metrics = [                     // порядок = индекс бита состояния
//     is_hungry = hunger > 0.5      // ключ = ds-выражение; tavl снимает 'is_hungry' и '=',
//     threat_present = threat_present //   остаток строки доедает devils_script::system → container
//   ]
//   actions = [
//     { name = think  effect = think  requirements = [ !is_hungry ]  next_state = [ resolved ]  weight_state = [ ] }
//   ]
//   goals = [ { name = resolved  requirements = [ ]  goal = [ resolved ] } ]
//
// Метрика ОПРЕДЕЛЯЕТ предикат прямо тут (скрипт компилируется и регистрируется под ключом — не нужно
// регистрировать предикаты отдельно в C++). В requirements/next_state префикс "!" / "not " = инверт.
// бит. Символические биты (не-метрики, напр. "resolved") индексируются после метрик.

namespace devils_engine {
namespace acumen {

struct goap_metric {
  std::string key;                  // имя предиката-метрики (регистрируется в act::registry, = бит)
  devils_script::container program; // скомпилированный ds-скрипт, вычисляющий метрику
  // resource id, где элемент ОПРЕДЕЛЁН (при overlay origin следует за перекрывшим ресурсом).
  // Потребителю с несколькими flatten-конфигами это даёт честный дедуп одноимённых регистраций:
  // одинаковый origin ⇒ копии одного скомпилированного скрипта, разный ⇒ конфликт имён.
  std::string origin;
};

struct goap_action_config {
  std::string name;                      // имя эффект-функции в act::registry
  std::vector<std::string> requirements; // ключи метрик (префикс !/not = инвертированный бит)
  std::vector<std::string> next_state;   // биты, которые ставит действие
  std::vector<std::string> weight_state; // биты веса (опц.)
  // Optional inline void devils_script program. The consumer registers an act::script_function<void>
  // under `name`; the resource only owns the compiled program and symbolic GOAP data.
  devils_script::container effect_program;
  bool has_effect_program = false;
  std::string origin; // см. goap_metric::origin
};

struct goap_goal_config {
  std::string name;
  std::vector<std::string> requirements;
  std::vector<std::string> goal;
};

struct goap_config {
  // Single-parent source inheritance. Merge is flattened before acumen::system construction:
  // base order is preserved, same-name entries replace in place, new entries append.
  std::string base;
  std::vector<goap_metric> metrics;
  std::vector<goap_action_config> actions;
  std::vector<goap_goal_config> goals;
  std::vector<std::string> disable_metrics;
  std::vector<std::string> disable_actions;
  std::vector<std::string> disable_goals;
};

goap_config merge_goap_config(const goap_config& base, const goap_config& derived);
goap_config resolve_goap_config(devils_engine::demiurg::resource_system& resources,
                                std::string_view id);

class goap_resource : public devils_engine::demiurg::resource_interface {
public:
  explicit goap_resource(const act::script_compiler* compiler);
  const goap_config& config() const noexcept {
    return config_;
  }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  const act::script_compiler* compiler_ = nullptr;
  goap_config config_;
};

} // namespace acumen
} // namespace devils_engine

#endif
