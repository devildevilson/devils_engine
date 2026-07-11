#ifndef TILE_FRONTIER_CORE_GOAP_RESOURCE_H
#define TILE_FRONTIER_CORE_GOAP_RESOURCE_H

#include <string>
#include <vector>

#include <devils_engine/demiurg/resource_base.h>
#include <devils_script/container.h>

namespace devils_script { struct system; }

// Дисковый конфиг GOAP-арбитра (acumen), парсится РУЧНЫМ проездом tavl-парсера (не deserialize —
// это не строгий агрегат). Формат:
//
//   metrics = [                     // порядок = индекс бита состояния
//     is_hungry = hunger > 0.5      // ключ = ds-выражение; tavl снимает 'is_hungry' и '=',
//     threat_present = threat_present //   остаток строки доедает devils_script::system → container
//   ]
//   actions = [
//     { name = eat  requirements = [ !threat_present, is_hungry ]  next_state = [ resolved ]  weight_state = [ ] }
//   ]
//   goals = [ { name = resolved  requirements = [ ]  goal = [ resolved ] } ]
//
// Метрика ОПРЕДЕЛЯЕТ предикат прямо тут (скрипт компилируется и регистрируется под ключом — не нужно
// регистрировать предикаты отдельно в C++). В requirements/next_state префикс "!" / "not " = инверт.
// бит. Символические биты (не-метрики, напр. "resolved") индексируются после метрик.

namespace tile_frontier {
namespace core {

struct goap_metric {
  std::string key;                    // имя предиката-метрики (регистрируется в act::registry, = бит)
  devils_script::container program;   // скомпилированный ds-скрипт, вычисляющий метрику
};

struct goap_action_config {
  std::string name;                       // имя эффект-функции в act::registry
  std::vector<std::string> requirements;  // ключи метрик (префикс !/not = инвертированный бит)
  std::vector<std::string> next_state;    // биты, которые ставит действие
  std::vector<std::string> weight_state;  // биты веса (опц.)
};

struct goap_goal_config {
  std::string name;
  std::vector<std::string> requirements;
  std::vector<std::string> goal;
};

struct goap_config {
  std::vector<goap_metric> metrics;
  std::vector<goap_action_config> actions;
  std::vector<goap_goal_config> goals;
};

class goap_resource : public devils_engine::demiurg::resource_interface {
public:
  explicit goap_resource(devils_script::system* sys);
  const goap_config& config() const noexcept { return config_; }

  void load_cold(const devils_engine::utils::safe_handle_t& handle) override;
  void load_warm(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_hot(const devils_engine::utils::safe_handle_t& handle) override;
  void unload_warm(const devils_engine::utils::safe_handle_t& handle) override;

private:
  devils_script::system* sys_ = nullptr; // заимствован (владелец — assets script_environment)
  goap_config config_;
};

}
}

#endif
