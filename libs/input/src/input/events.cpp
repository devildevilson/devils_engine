#include "events.h"

#include "devils_engine/utils/time-utils.hpp"
#include "core.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace input {

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
}

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
}

void events::init() {
  // что тут? тут следует подгрузить клавиши из настроек
  // + к этому их обратно туда записать 
}

constexpr uint32_t click_mask = event_state::click | event_state::long_click;

void events::update(const size_t time) {
  for (auto &[scancode, d] : key_mapping) {
    const bool state_changed = d.key_time == 0;
    const auto prev = d.prev;
    const auto key_time = d.key_time;
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

        d.current = press ? d.current = event_state::press : d.current;
        d.press_event_time = 0;
        break;
      }

      case event_state::press: {
        if (press && press_event_time >= long_press_duration) {
          d.prev = d.current;
          d.current = event_state::long_press;
          break;
        }

        d.current = !press ? d.current = event_state::click : d.current;
        d.click_event_time = 0;
        break;
      }

      case event_state::long_press: {
        d.current = !press ? d.current = event_state::long_click : d.current;
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

        d.current = !press ? d.current = event_state::double_click : d.current;
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
  if (itr == key_mapping.end()) return;

  if (itr->second.state != static_cast<key_state::values>(state)) itr->second.key_time = 0;
  itr->second.state = static_cast<key_state::values>(state);
}

bool events::is_pressed(const std::string_view &id) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) return false;

  for (const auto scancode : itr->second.keys) {
    const auto itr = key_mapping.find(scancode);
    if (scancode == -1 || itr == key_mapping.end()) continue;

    if (itr->second.state > key_state::release) return true;
  }

  return false;
}

bool events::is_released(const std::string_view &id) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) return false;

  for (const auto scancode : itr->second.keys) {
    const auto itr = key_mapping.find(scancode);
    if (scancode == -1 || itr == key_mapping.end()) continue;

    if (itr->second.state > key_state::release) return false;
  }

  return true;
}

std::string_view events::key_name(const int32_t key, const int32_t scancode) {
  return input::key_name(key, scancode);
}

std::string_view events::key_name(const std::string_view &id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) return std::string_view();

  if (slot >= itr->second.keys.size()) return std::string_view();
  if (itr->second.keys[slot] == -1) return std::string_view();

  const int32_t scancode = itr->second.keys[slot];
  return input::key_name(-1, scancode);
}

std::string events::key_name_native(const int32_t key, const int32_t scancode) {
  return input::key_name_native(key, scancode);
}

std::string events::key_name_native(const std::string_view &id, const uint8_t slot) {
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) return std::string();

  if (slot >= itr->second.keys.size()) return std::string();
  if (itr->second.keys[slot] == -1) return std::string();

  const int32_t scancode = itr->second.keys[slot];
  return input::key_name_native(-1, scancode);
}

void events::set_key(const std::string_view &id, const int32_t scancode, const int32_t key, const uint8_t slot) {
  // тут нужно убедиться что это либо дефолтный ивент либо добавить его в список
  const auto persistend_id = make_persistent_event_id(id);
  auto itr = event_mapping.find(persistend_id);
  if (itr == event_mapping.end()) {
    itr = event_mapping.insert(std::make_pair(persistend_id, event_map{})).first;
  }

  if (slot >= itr->second.keys.size()) utils::error{}("Bad set key args. Trying to set to event '{}' slot {}", id, slot);
  const auto old_scancode = itr->second.keys[slot];
  itr->second.keys[slot] = scancode;

  auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) {
    key_itr = key_mapping.insert(std::make_pair(scancode, key_data{})).first;
    key_itr->second.glfw_key = key;
  }

  for (auto &name : key_itr->second.events) {
    if (name.empty()) { name = persistend_id; break; }
  }

  auto old_key_itr = key_mapping.find(old_scancode);
  if (old_key_itr != key_mapping.end()) {
    for (auto &name : old_key_itr->second.events) {
      if (name == persistend_id) { name = std::string_view(); break; }
    }
  }

  // обновим настройки
}

const std::array<std::string_view, 16> &events::mapping(const int32_t scancode) {
  const auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) return std::array<std::string_view, 16>();

  return key_itr->second.events;
}

event_state::values events::current_event_state(const int32_t scancode) {
  const auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) return event_state::release;

  return key_itr->second.current;
}

uint32_t events::current_event_state(const std::string_view &id) {
  uint32_t mask = 0;
  
  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) return mask;

  // может надо сначала вернуть наибольшее значение? вряд ли
  for (const auto scancode : itr->second.keys) {
    const auto state = static_cast<uint32_t>(current_event_state(scancode));
    mask = mask | state;
  }

  return mask;
}

event_state::values events::max_event_state(const std::string_view &id) {
  event_state::values max = event_state::values::release;

  const auto itr = event_mapping.find(id);
  if (itr == event_mapping.end()) return max;

  for (const auto scancode : itr->second.keys) {
    const auto state = current_event_state(scancode);
    max = std::max(max, state);
  }

  return max;
}

bool events::check_key(const int32_t scancode, const uint32_t states) {
  const auto current = current_event_state(scancode);
  const auto key_event_state = static_cast<uint32_t>(current);
  return (key_event_state & states) != 0;
}

bool events::timed_check_key(const int32_t scancode, const uint32_t states, const size_t wait, const size_t period) {
  const auto key_itr = key_mapping.find(scancode);
  if (key_itr == key_mapping.end()) return false;

  // нужно взять время, какое время откуда
  // это время для событий долгого нажатия
  // вместо того чтобы в каждом кадре вызывать действие
  // мы будем вызывать действие через период

  // тут нужно указать время тика, типа если время ивента < чем время одного тика тогда возвращается true
  if (key_itr->second.press_event_time < wait) return false;
  if (key_itr->second.press_event_time % period >= engine_tick_time) return false;

  const auto key_event_state = static_cast<uint32_t>(key_itr->second.current);
  return (key_event_state & states) != 0;
}

bool events::check_event(const std::string_view &event, const uint32_t states) {
  const auto itr = event_mapping.find(event);
  if (itr == event_mapping.end()) return false;

  // может надо сначала вернуть наибольшее значение? вряд ли
  for (const auto scancode : itr->second.keys) {
    if (check_key(scancode, states)) return true;
  }

  return false;
}

bool events::timed_check_event(const std::string_view &event, const uint32_t states, const size_t wait, const size_t period) {
  const auto itr = event_mapping.find(event);
  if (itr == event_mapping.end()) return false;

  for (const auto scancode : itr->second.keys) {
    if (timed_check_key(scancode, states, wait, period)) return true;
  }

  return false;
}

void events::set_long_press_duration(const size_t dur) { long_press_duration = dur; }
void events::set_double_press_duration(const size_t dur) { double_press_duration = dur; }
void events::set_engine_tick_time(const size_t time) { engine_tick_time = time; }

std::string_view events::make_persistent_event_id(const std::string_view &id) {
  auto itr = additional_events.find(id);
  if (itr == additional_events.end()) {
    itr = additional_events.emplace(std::string(id)).first;
  }
  
  return std::string_view(*itr);
}

struct events::auxiliary &events::auxiliary_data() { return auxiliary; }

size_t events::time = 0;
size_t events::long_press_duration = utils::app_clock::resolution() / 3;
size_t events::double_press_duration = utils::app_clock::resolution() / 3;
size_t events::engine_tick_time = utils::app_clock::resolution() / 30; // 30fps
struct events::auxiliary events::auxiliary;
gtl::flat_hash_map<std::string_view, events::event_map> events::event_mapping;
gtl::flat_hash_map<int32_t, events::key_data> events::key_mapping;  // scancodes
gtl::flat_hash_set<std::string> events::additional_events;
}
}