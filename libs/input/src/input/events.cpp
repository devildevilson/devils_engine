#include "core.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/time-utils.hpp"
#include "events.h"

namespace devils_engine {
namespace input {

namespace default_events {
std::string_view to_string(const values v) {
  return static_cast<size_t>(v) < values::count ? names[static_cast<size_t>(v)] : std::string_view();
}
} // namespace default_events

events::event_map::event_map() noexcept {
  keys.fill(-1);
}

events::key_data::key_data()
  : current(event_state::values::release),
    prev(event_state::values::release),
    state(key_state::values::release),
    press_event_time(0),
    click_event_time(0),
    key_time(0),
    glfw_key(-1) {
  events.fill(invalid_event_id);
}

events::auxiliary::auxiliary()
  : mouse_wheel(0.0f),
    current_text(0),
    text{0},
    last_frame_time(0),
    click_pos_x(0),
    click_pos_y(0),
    fb_scale_x(0.0f),
    fb_scale_y(0.0f) {}

namespace key_state {
std::string_view to_string(const values v) {
  switch (v) {
    case values::release: return "release";
    case values::press: return "press";
    case values::repeated: return "repeated";
    default: break;
  }

  return std::string_view();
}
} // namespace key_state

namespace event_state {
std::string_view to_string(const values v) {
  switch (v) {
    case values::release: return "release";
    case values::press: return "press";
    case values::long_press: return "long_press";
    case values::click: return "click";
    case values::long_click: return "long_click";
    case values::double_press: return "double_press";
    case values::double_click: return "double_click";
    default: break;
  }

  return std::string_view();
}
} // namespace event_state

void events::init() {
  // что тут? тут следует подгрузить клавиши из настроек
  // + к этому их обратно туда записать
}

constexpr uint32_t click_mask = event_state::click | event_state::long_click;

void events::update(const size_t time) {
  for (auto& [scancode, d] : key_mapping) {
    const bool state_changed = d.key_time == 0;
    const auto prev = d.prev;
    //const auto key_time = d.key_time;
    const auto press_event_time = d.press_event_time;
    const auto click_event_time = d.click_event_time;

    d.key_time += time;
    d.press_event_time += time;
    d.click_event_time += time;

    if (state_changed) {
      d.prev = d.current;
      d.key_time = 0;
    }

    const bool press = d.state != key_state::release;
    switch (d.current) {
      case event_state::release: {
        if (press && ((prev & click_mask) != 0) && click_event_time <= double_press_duration) {
          d.current = event_state::double_press;
          d.press_event_time = 0;
          break;
        }

        d.current = press ? event_state::press : d.current;
        d.press_event_time = 0;
        break;
      }

      case event_state::press: {
        if (press && press_event_time >= long_press_duration) {
          d.prev = d.current;
          d.current = event_state::long_press;
          break;
        }

        d.current = !press ? event_state::click : d.current;
        d.click_event_time = 0;
        break;
      }

      case event_state::long_press: {
        d.current = !press ? event_state::long_click : d.current;
        d.click_event_time = 0;
        break;
      }

      case event_state::click: {
        d.current = event_state::release;
        break;
      }

      case event_state::long_click: {
        d.current = event_state::release;
        break;
      }

      case event_state::double_press: {
        if (press && press_event_time >= long_press_duration) {
          d.prev = d.current;
          d.current = event_state::long_press;
          break;
        }

        d.current = !press ? event_state::double_click : d.current;
        d.click_event_time = 0;
        break;
      }

      case event_state::double_click: {
        d.current = event_state::release;
        break;
      }
    }
  }
}

void events::update_key(const int32_t scancode, const int32_t state) {
  auto itr = key_mapping.find(scancode);
  if (itr == key_mapping.end()) {
    return;
  }

  if (itr->second.state != static_cast<key_state::values>(state)) {
    itr->second.key_time = 0;
  }
  itr->second.state = static_cast<key_state::values>(state);
}

// кнопки мыши — first-class через синтетический сканкод (см. mouse_button_scancode).
void events::update_mouse_button(const int32_t button, const int32_t state) {
  update_key(mouse_button_scancode(button), state);
}

void events::set_mouse_button(const event_id id, const int32_t button, const uint8_t slot) {
  set_key(id, mouse_button_scancode(button), -1, slot);
}

void events::set_mouse_button(const std::string_view& id, const int32_t button, const uint8_t slot) {
  set_key(id, mouse_button_scancode(button), -1, slot);
}

bool events::is_pressed(const event_id id) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return false;
  }

  for (const auto scancode : itr->second.keys) {
    const auto itr = key_mapping.find(scancode);
    if (scancode == -1 || itr == key_mapping.end()) {
      continue;
    }

    if (itr->second.state > key_state::release) {
      return true;
    }
  }

  return false;
}

bool events::is_pressed(const std::string_view& id) {
  return is_pressed(make_event_id(id));
}

bool events::is_released(const event_id id) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return false;
  }

  for (const auto scancode : itr->second.keys) {
    const auto itr = key_mapping.find(scancode);
    if (scancode == -1 || itr == key_mapping.end()) {
      continue;
    }

    if (itr->second.state > key_state::release) {
      return false;
    }
  }

  return true;
}

bool events::is_released(const std::string_view& id) {
  return is_released(make_event_id(id));
}

std::string_view events::key_name(const int32_t key, const int32_t scancode) {
  return input::key_name(key, scancode);
}

std::string_view events::key_name(const event_id id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return std::string_view();
  }

  if (slot >= itr->second.keys.size()) {
    return std::string_view();
  }
  if (itr->second.keys[slot] == -1) {
    return std::string_view();
  }

  const int32_t scancode = itr->second.keys[slot];
  return input::key_name(-1, scancode);
}

std::string_view events::key_name(const std::string_view& id, const uint8_t slot) {
  return key_name(make_event_id(id), slot);
}

std::string_view events::key_name_canonical(const int32_t scancode) {
  return input::key_name_canonical(scancode);
}

std::string_view events::key_name_canonical(const event_id id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return std::string_view();
  }

  if (slot >= itr->second.keys.size()) {
    return std::string_view();
  }
  if (itr->second.keys[slot] == -1) {
    return std::string_view();
  }

  return input::key_name_canonical(itr->second.keys[slot]);
}

std::string_view events::key_name_canonical(const std::string_view& id, const uint8_t slot) {
  return key_name_canonical(make_event_id(id), slot);
}

std::string_view events::key_name_us_layout(const int32_t scancode) {
  return input::key_name_us_layout(scancode);
}

std::string_view events::key_name_us_layout(const event_id id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return std::string_view();
  }

  if (slot >= itr->second.keys.size()) {
    return std::string_view();
  }
  if (itr->second.keys[slot] == -1) {
    return std::string_view();
  }

  return input::key_name_us_layout(itr->second.keys[slot]);
}

std::string_view events::key_name_us_layout(const std::string_view& id, const uint8_t slot) {
  return key_name_us_layout(make_event_id(id), slot);
}

std::string events::key_name_local(const int32_t scancode) {
  return input::key_name_local(scancode);
}

std::string events::key_name_local(const event_id id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return std::string();
  }

  if (slot >= itr->second.keys.size()) {
    return std::string();
  }
  if (itr->second.keys[slot] == -1) {
    return std::string();
  }

  return input::key_name_local(itr->second.keys[slot]);
}

std::string events::key_name_local(const std::string_view& id, const uint8_t slot) {
  return key_name_local(make_event_id(id), slot);
}

std::string events::key_name_native(const int32_t key, const int32_t scancode) {
  return input::key_name_native(key, scancode);
}

std::string events::key_name_native(const event_id id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return std::string();
  }

  if (slot >= itr->second.keys.size()) {
    return std::string();
  }
  if (itr->second.keys[slot] == -1) {
    return std::string();
  }

  const int32_t scancode = itr->second.keys[slot];
  return input::key_name_native(-1, scancode);
}

std::string events::key_name_native(const std::string_view& id, const uint8_t slot) {
  return key_name_native(make_event_id(id), slot);
}

void events::set_key(const event_id id, const int32_t scancode, const int32_t key, const uint8_t slot) {
  auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    itr = event_mapping.insert(std::make_pair(id, event_map{})).first;
  }

  if (slot >= itr->second.keys.size()) {
    utils::error{}("Bad set key args. Trying to set to event '{}' slot {}", id, slot);
  }
  const auto old_scancode = itr->second.keys[slot];
  itr->second.keys[slot] = scancode;

  auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) {
    key_itr = key_mapping.insert(std::make_pair(scancode, key_data{})).first;
    key_itr->second.glfw_key = key;
  }

  for (auto& name : key_itr->second.events) {
    if (name == invalid_event_id) {
      name = id;
      break;
    }
  }

  detach_event(id, old_scancode);
}

void events::detach_event(const event_id id, const int32_t scancode) {
  auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) {
    return;
  }

  for (auto& name : key_itr->second.events) {
    if (name == id) {
      name = invalid_event_id;
      break;
    }
  }

  bool empty = true;
  for (const auto name : key_itr->second.events) {
    if (name != invalid_event_id) {
      empty = false;
      break;
    }
  }

  if (empty) {
    key_mapping.erase(key_itr);
  }
}

void events::clear_key(const event_id id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return;
  }
  if (slot >= itr->second.keys.size()) {
    return;
  }

  const auto old_scancode = itr->second.keys[slot];
  if (old_scancode == -1) {
    return;
  }
  itr->second.keys[slot] = -1;
  detach_event(id, old_scancode);
}

void events::clear_event(const event_id id) {
  for (uint8_t slot = 0; slot < event_key_slots_count; ++slot) {
    clear_key(id, slot);
  }
}

void events::clear_event(const std::string_view& id) {
  clear_event(make_event_id(id));
}

void events::clear_bindings() {
  event_mapping.clear();
  key_mapping.clear();
}

bool events::has_bindings() {
  return !event_mapping.empty();
}

void events::for_each_event(void (*cb)(void* user, const std::string_view name, const std::span<const int32_t> keys), void* user) {
  for (const auto& [id, map] : event_mapping) {
    cb(user, std::string_view(map.name), std::span<const int32_t>(map.keys.data(), map.keys.size()));
  }
}

void events::set_key(const std::string_view& id, const int32_t scancode, const int32_t key, const uint8_t slot) {
  const auto hash = make_event_id(id);
  auto itr = event_mapping.find(hash);
  if (itr == event_mapping.end()) {
    event_map map;
    map.name = std::string(id);
    event_mapping.emplace(hash, map);
  } else if (itr->second.name.empty()) {
    itr->second.name = std::string(id);
  } else if (std::string_view(itr->second.name) != id) {
    utils::error{}("Input event hash collision: '{}' vs '{}'", itr->second.name, id);
  }

  set_key(hash, scancode, key, slot);
}

std::span<const events::event_id> events::mapping(const int32_t scancode) {
  const auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) {
    return std::span<const event_id>();
  }

  return std::span<const event_id>(key_itr->second.events.data(), key_itr->second.events.size());
}

event_state::values events::current_event_state(const int32_t scancode) {
  const auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) {
    return event_state::release;
  }

  return key_itr->second.current;
}

uint32_t events::current_event_state(const event_id id) {
  uint32_t mask = 0;

  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return mask;
  }

  // может надо сначала вернуть наибольшее значение? вряд ли
  for (const auto scancode : itr->second.keys) {
    const auto state = static_cast<uint32_t>(current_event_state(scancode));
    mask = mask | state;
  }

  return mask;
}

uint32_t events::current_event_state(const std::string_view& id) {
  return current_event_state(make_event_id(id));
}

event_state::values events::max_event_state(const event_id id) {
  event_state::values max = event_state::values::release;

  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) {
    return max;
  }

  for (const auto scancode : itr->second.keys) {
    const auto state = current_event_state(scancode);
    max = std::max(max, state);
  }

  return max;
}

event_state::values events::max_event_state(const std::string_view& id) {
  return max_event_state(make_event_id(id));
}

bool events::check_key(const int32_t scancode, const uint32_t states) {
  const auto current = current_event_state(scancode);
  const auto key_event_state = static_cast<uint32_t>(current);
  return (key_event_state & states) != 0;
}

bool events::timed_check_key(const int32_t scancode, const uint32_t states, const size_t wait, const size_t period) {
  const auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) {
    return false;
  }

  // нужно взять время, какое время откуда
  // это время для событий долгого нажатия
  // вместо того чтобы в каждом кадре вызывать действие
  // мы будем вызывать действие через период

  // тут нужно указать время тика, типа если время ивента < чем время одного тика тогда возвращается true
  if (key_itr->second.press_event_time < wait) {
    return false;
  }
  if (key_itr->second.press_event_time % period >= engine_tick_time) {
    return false;
  }

  const auto key_event_state = static_cast<uint32_t>(key_itr->second.current);
  return (key_event_state & states) != 0;
}

bool events::check_event(const event_id event, const uint32_t states) {
  const auto itr = event_mapping.find(event);
  if (itr == event_mapping.end()) {
    return false;
  }

  // может надо сначала вернуть наибольшее значение? вряд ли
  for (const auto scancode : itr->second.keys) {
    if (check_key(scancode, states)) {
      return true;
    }
  }

  return false;
}

bool events::check_event(const std::string_view& event, const uint32_t states) {
  return check_event(make_event_id(event), states);
}

bool events::timed_check_event(const event_id event, const uint32_t states, const size_t wait, const size_t period) {
  const auto itr = event_mapping.find(event);
  if (itr == event_mapping.end()) {
    return false;
  }

  for (const auto scancode : itr->second.keys) {
    if (timed_check_key(scancode, states, wait, period)) {
      return true;
    }
  }

  return false;
}

bool events::timed_check_event(const std::string_view& event, const uint32_t states, const size_t wait, const size_t period) {
  return timed_check_event(make_event_id(event), states, wait, period);
}

void events::set_long_press_duration(const size_t dur) {
  long_press_duration = dur;
}
void events::set_double_press_duration(const size_t dur) {
  double_press_duration = dur;
}
void events::set_engine_tick_time(const size_t time) {
  engine_tick_time = time;
}

events::event_id events::make_event_id(const std::string_view& id) noexcept {
  return utils::string_hash(id);
}

struct events::auxiliary& events::auxiliary_data() {
  return auxiliary;
}

size_t events::time = 0;
size_t events::long_press_duration = utils::app_clock::resolution() / 3;
size_t events::double_press_duration = utils::app_clock::resolution() / 3;
size_t events::engine_tick_time = utils::app_clock::resolution() / 30; // 30fps
struct events::auxiliary events::auxiliary;
gtl::flat_hash_map<events::event_id, events::event_map> events::event_mapping;
gtl::flat_hash_map<int32_t, events::key_data> events::key_mapping; // scancodes
} // namespace input
} // namespace devils_engine
