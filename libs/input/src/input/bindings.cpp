#include <algorithm>
#include <charconv>

#include "bindings.h"
#include "devils_engine/utils/core.h"
#include "events.h"
#include "key_names.h"

namespace devils_engine {
namespace input {

namespace {
// GLFW mouse buttons 0..7; имена стабильны и не зависят от платформы/раскладки.
constexpr std::string_view mouse_button_names[] = {
  "mouse_left", "mouse_right", "mouse_middle",
  "mouse_4", "mouse_5", "mouse_6", "mouse_7", "mouse_8",
};
constexpr std::string_view scancode_prefix = "scancode_";
} // namespace

std::tuple<int32_t, int32_t> binding_key_from_name(const std::string_view& name) noexcept {
  for (size_t i = 0; i < std::size(mouse_button_names); ++i) {
    if (mouse_button_names[i] == name) {
      return std::make_tuple(-1, events::mouse_button_scancode(int32_t(i)));
    }
  }

  if (name.starts_with(scancode_prefix)) {
    const auto digits = name.substr(scancode_prefix.size());
    int32_t scancode = -1;
    const auto [ptr, ec] = std::from_chars(digits.data(), digits.data() + digits.size(), scancode);
    if (ec == std::errc{} && ptr == digits.data() + digits.size() && scancode >= 0) {
      return std::make_tuple(-1, scancode);
    }
    return std::make_tuple(-1, -1);
  }

  return get_key_from_canonical(name);
}

std::string binding_name_from_scancode(const int32_t scancode) {
  if (scancode < 0) {
    return std::string();
  }

  if (scancode >= events::mouse_button_scancode_base) {
    const int32_t button = scancode - events::mouse_button_scancode_base;
    if (size_t(button) < std::size(mouse_button_names)) {
      return std::string(mouse_button_names[button]);
    }
  } else {
    const auto canonical = get_key_name_canonical(scancode);
    if (!canonical.empty()) {
      return std::string(canonical);
    }
  }

  // lossless-fallback: кнопка вне canonical-таблицы переживает save/load сырым сканкодом
  return std::string(scancode_prefix) + std::to_string(scancode);
}

void apply_bindings(const bindings_config& config) {
  for (const auto& [action, keys] : config.actions) {
    if (keys.size() > events::event_key_slots_count) {
      utils::warn("input bindings: action '{}' lists {} keys, only {} slots are supported",
                  action, keys.size(), events::event_key_slots_count);
    }

    const size_t count = std::min(keys.size(), events::event_key_slots_count);
    uint8_t slot = 0;
    for (size_t i = 0; i < count; ++i) {
      const auto [glfw_key, scancode] = binding_key_from_name(keys[i]);
      if (scancode < 0) {
        utils::warn("input bindings: unknown key '{}' for action '{}'", keys[i], action);
        continue;
      }
      events::set_key(std::string_view(action), scancode, glfw_key, slot);
      slot += 1;
    }

    // Перепривязка целиком: хвостовые слоты чистим (пустой список = полная отвязка действия).
    for (; slot < events::event_key_slots_count; ++slot) {
      events::clear_key(events::make_event_id(action), slot);
    }
  }
}

namespace {
void collect_event(void* user, const std::string_view name, const std::span<const int32_t> keys) {
  auto& out = *static_cast<bindings_config*>(user);
  if (name.empty()) {
    return; // событие без имени (голый hash) в текстовый конфиг не выгрузить
  }

  auto& list = out.actions[std::string(name)];
  list.clear();
  for (const auto scancode : keys) {
    if (scancode < 0) {
      continue;
    }
    auto key_name = binding_name_from_scancode(scancode);
    if (!key_name.empty()) {
      list.push_back(std::move(key_name));
    }
  }
}
} // namespace

bindings_config collect_bindings() {
  bindings_config out;
  events::for_each_event(&collect_event, &out);
  return out;
}

} // namespace input
} // namespace devils_engine
