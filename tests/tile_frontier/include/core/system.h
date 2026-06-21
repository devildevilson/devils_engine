#ifndef TILE_FRONTIER_CORE_SYSTEM_H
#define TILE_FRONTIER_CORE_SYSTEM_H

#include <memory>
#include <string>
#include <devils_engine/simul/interface.h>
#include <devils_engine/utils/actor_ref.h>
#include <devils_engine/utils/type_traits.h>

/*
что тут? тут мы наверное хотим определить систему которую запустим в main.cpp
для этой системы будет определен actor_ref и где то он должен быть глобально доступен
как же это все соединить
*/

namespace tile_frontier {
namespace core {

using namespace devils_engine;

struct simulation_init;
struct sound_simulation_init;
struct render_simulation_init;
struct assets_simulation_init;

constexpr size_t seq_simul_type_id = 0xaa1;
constexpr size_t seq_sound_type_id = 0xaa2;
constexpr size_t seq_graphics_type_id = 0xaa3;
constexpr size_t seq_assets_type_id = 0xaa4;

using simulation_actor = utils::actor_ref<seq_simul_type_id>;
using sound_actor = utils::actor_ref<seq_sound_type_id>;
using graphics_actor = utils::actor_ref<seq_graphics_type_id>;
using assets_actor = utils::actor_ref<seq_assets_type_id>;

struct task_id_t {};
inline size_t generate_task_id() noexcept { return utils::sequential_type_id<0, task_id_t>(); };

struct render_simulation_config {
  std::string render_config_folder;
  std::string pipeline_cache_path;
  std::string graph_name = "graphics1";
  bool create_vulkan_on_init = true;
  bool headless = false;
};

//simulation_actor* g_core_actor = nullptr;
//sound_actor* g_sound_actor = nullptr;
//graphics_actor* g_graphics_actor = nullptr;
//assets_actor* g_assets_actor = nullptr;

// тут мы создадим звук, графику, ассеты
// типовые реализации систем могут быть в основном движке?
// вообще потенциально могут но надо будет тогда зафиксировать что есть что
// расширять сложно будет, лучше все таки оставить те вещи в качестве инструментария
// пока что неочевидны контракты между системами...
// может быть можно будет потом добавить
class simulation : public simul::advancer {
public:
  simulation() noexcept;
  ~simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;
private:
  std::unique_ptr<simulation_init> container;
  simulation_actor actor;

  sound_actor* sactor;
  graphics_actor* gactor;
  assets_actor* aactor;
};

// звук делает что? 
class sound_simulation : public simul::advancer {
public:
  sound_simulation(const size_t frame_time) noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  sound_actor* get_actor();
private:
  std::unique_ptr<sound_simulation_init> container;
  sound_actor actor;
};

class render_simulation : public simul::advancer {
public:
  render_simulation(const size_t frame_time, render_simulation_config config) noexcept;
  ~render_simulation() noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  graphics_actor* get_actor();
private:
  std::unique_ptr<render_simulation_init> container;
  graphics_actor actor;
};

class assets_simulation : public simul::advancer {
public:
  assets_simulation(const size_t frame_time) noexcept;
  void init() override;
  bool stop_predicate() const override;
  void update(const size_t time) override;

  assets_actor* get_actor();
private:
  std::unique_ptr<assets_simulation_init> container;
  assets_actor actor;
};

}
}

#endif
