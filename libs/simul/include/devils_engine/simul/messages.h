#ifndef DEVILS_ENGINE_SIMUL_MESSAGES_H
#define DEVILS_ENGINE_SIMUL_MESSAGES_H

// POD-like commands exchanged by the standard simulation broker channels.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <devils_engine/demiurg/resource_system.h>
#include <devils_engine/utils/core.h>

struct GLFWwindow;
struct GLFWmonitor;

namespace devils_engine {
namespace simul {

// Уникальный монотонный id задачи НА КАЖДЫЙ вызов (не actor/type id). Broker-консьюмеры
// (напр. sound::system2) дедуплицируют задачи по нему, поэтому всем сообщениям нужен монотонный
// источник; счётчик atomic, т.к. id раздают sim/main потоки.
inline size_t generate_task_id() noexcept {
  static std::atomic<size_t> counter{0};
  return counter.fetch_add(1, std::memory_order_relaxed);
}

struct resource_ref {
  demiurg::resource_handle handle;
  demiurg::resource_interface* direct = nullptr;

  static resource_ref from_handle(const demiurg::resource_handle h) noexcept {
    resource_ref ref;
    ref.handle = h;
    return ref;
  }

  static resource_ref from_direct(demiurg::resource_interface* const ptr) noexcept {
    resource_ref ref;
    ref.direct = ptr;
    return ref;
  }

  static resource_ref from_system(const demiurg::resource_system* const system, demiurg::resource_interface* const ptr) noexcept {
    if (system != nullptr && ptr != nullptr) {
      const demiurg::resource_handle h = system->handle(ptr->id);
      if (h.get() == ptr) {
        return from_handle(h);
      }
    }
    return from_direct(ptr);
  }

  demiurg::resource_interface* get() const noexcept {
    if (auto* ptr = handle.get()) {
      return ptr;
    }
    return direct;
  }

  template <typename T>
  T* get() const noexcept {
    if (auto* ptr = handle.template get<T>()) {
      return ptr;
    }
    if (direct == nullptr || !direct->is_type(utils::type_id<T>())) {
      return nullptr;
    }
    return static_cast<T*>(direct);
  }
};

struct command_window_recreation {
  GLFWwindow* w = nullptr;
  GLFWmonitor* m = nullptr;
  uint32_t width = 0;
  uint32_t height = 0;
};

struct command_window_resize {
  uint32_t width = 0;
  uint32_t height = 0;
};

struct command_render_set_active {
  bool draw = true;
};

struct command_render_set_graph {
  std::string name;
};

struct command_render_update_constant {
  std::string name;
  std::vector<uint8_t> bytes;
};

struct command_load_resource {
  resource_ref res;
  int32_t target = 0;
};

struct command_gpu_transition {
  resource_ref res;
  bool load = true;
};

struct command_gpu_done {
  resource_ref res;
};

struct command_prepare_shaders {
  const demiurg::resource_system* registry = nullptr;
  std::string prefix;
};

struct command_shaders_prepared {
  uint32_t compiled = 0;
  uint32_t failed = 0;
};

struct command_sound_play {
  size_t taskid = 0;
  size_t after = SIZE_MAX;
  resource_ref res;
  double start = 0.0;
  uint32_t type = UINT32_MAX;
};

struct command_sound_stop {
  size_t taskid = 0;
};

struct command_sound_update {
  size_t taskid = 0;
  float pos[3] = {};
  float dir[3] = {};
  float vel[3] = {};
};

struct command_sound_set_master_gain {
  float gain = 1.0f;
};

struct command_recreate_sound_system {
  std::string device_name;
};

struct command_sound_devices {
  size_t request_id = 0;
  std::vector<std::string>* out = nullptr;
  std::atomic_bool* ready = nullptr;
};

struct sound_state_entry {
  size_t taskid = 0;
  double progress = 0.0;
};

struct command_sound_state {
  std::vector<sound_state_entry> sounds;
};

} // namespace simul
} // namespace devils_engine

#endif
