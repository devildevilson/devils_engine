#include "devils_engine/flow/system.h"

#include <algorithm>
#include <cmath>
#include <optional>

#include <tavl/deserialize.h>

#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace flow {
namespace {

constexpr float pi_v = 3.14159265358979323846f;
constexpr float tau_v = pi_v * 2.0f;

struct state_mirror {
  uint64_t duration = 0;
  std::optional<std::string> next;
  std::vector<std::string> images;
  std::optional<std::string> action;
  std::vector<float> uv;
};

static bool is_mirror_token(const std::string_view s) noexcept {
  return s == "u" || s == "v" || s == "uv";
}

static size_t find_last_colon(const std::string_view s) noexcept {
  return s.find_last_of(':');
}

static state convert_state(
  const state_mirror& src,
  const demiurg::resource_system* resources,
  const std::string_view label,
  const size_t index,
  const parse_options& options
) {
  state out;
  out.duration_mcs = src.duration;

  if (src.action.has_value() && !src.action->empty()) {
    out.action = utils::string_hash(*src.action);
  }

  if (src.uv.size() >= 2) {
    out.uv = vec2{src.uv[0], src.uv[1]};
    if (src.uv.size() > 2) {
      utils::warn("flow '{}:{}': uv has {} values, only first two are used", label, index, src.uv.size());
    }
  } else if (!src.uv.empty()) {
    utils::warn("flow '{}:{}': uv needs two values, got {}", label, index, src.uv.size());
  }

  out.images.reserve(src.images.size());
  for (const auto& img : src.images) {
    out.images.push_back(parse_image_ref(img, resources));
  }

  if (options.warn_on_odd_direction_count) {
    const auto count = out.images.size();
    if (count != 0 && count != 1 && count != 4 && count != 8 && count != 16) {
      utils::warn("flow '{}:{}': images count {} is allowed but unusual (expected 1/4/8/16)", label, index, count);
    }
  }

  return out;
}

static void emit_action_once(const state& st, const uint32_t state_index, playback& pb, sample_result& out) {
  if (pb.action_emitted || st.action == utils::invalid_id) return;
  out.actions.push_back(action_event{state_index, st.action});
  pb.action_emitted = true;
}

static void enter_state(playback& pb, const uint32_t next) {
  pb.current = next;
  pb.elapsed_mcs = 0;
  pb.action_emitted = false;
  pb.finished = next == invalid_state;
}

}

uint32_t library::add_state(std::string name, state st) {
  const uint32_t existing = find_state(name);
  if (existing != invalid_state) {
    utils::warn("flow: state '{}' is already registered at index {}, replacing data", name, existing);
    states_[existing].data = std::move(st);
    return existing;
  }

  const uint32_t index = static_cast<uint32_t>(states_.size());
  states_.push_back(named_state{std::move(name), std::move(st)});
  resolve_pending_links(false);
  return index;
}

uint32_t library::find_state(const std::string_view name) const noexcept {
  for (uint32_t i = 0; i < states_.size(); ++i) {
    if (states_[i].name == name) return i;
  }
  return invalid_state;
}

const state* library::get(const uint32_t index) const noexcept {
  return index < states_.size() ? &states_[index].data : nullptr;
}

state* library::get(const uint32_t index) noexcept {
  return index < states_.size() ? &states_[index].data : nullptr;
}

size_t library::size() const noexcept {
  return states_.size();
}

bool library::set_next(const uint32_t index, const std::string_view next_name) {
  if (index >= states_.size() || next_name.empty()) return false;

  const uint32_t next_index = find_state(next_name);
  if (next_index != invalid_state) {
    states_[index].data.next = next_index;
    return true;
  }

  pending_.push_back(pending_link{index, std::string(next_name)});
  return false;
}

uint32_t library::append_resource_states(
  const std::string_view resource_id,
  const std::vector<state>& states,
  const std::vector<std::string>& next_names
) {
  const uint32_t first = static_cast<uint32_t>(states_.size());

  for (size_t i = 0; i < states.size(); ++i) {
    add_state(std::string(resource_id) + ":" + std::to_string(i), states[i]);
  }

  for (size_t i = 0; i < next_names.size() && i < states.size(); ++i) {
    if (next_names[i].empty()) continue;
    set_next(first + static_cast<uint32_t>(i), next_names[i]);
  }

  resolve_pending_links(false);
  return first;
}

void library::resolve_pending_links(const bool warn_unresolved) {
  size_t write = 0;
  for (size_t i = 0; i < pending_.size(); ++i) {
    const auto& p = pending_[i];
    if (p.from >= states_.size()) continue;

    const uint32_t next_index = find_state(p.next);
    if (next_index != invalid_state) {
      states_[p.from].data.next = next_index;
      continue;
    }

    if (warn_unresolved) {
      utils::warn("flow: unresolved next state '{}' from state '{}'", p.next, states_[p.from].name);
    }
    pending_[write++] = p;
  }
  pending_.resize(write);
}

sample_result library::sample(
  playback& pb,
  const uint64_t dt_mcs,
  const sample_context& ctx,
  const uint32_t zero_duration_step_limit
) const {
  sample_result out;
  if (pb.current == invalid_state || pb.current >= states_.size()) {
    pb.finished = true;
    return out;
  }

  uint64_t remaining = dt_mcs;
  uint32_t guard = 0;

  while (pb.current != invalid_state && pb.current < states_.size()) {
    const state& st = states_[pb.current].data;
    const uint32_t cur_index = pb.current;
    emit_action_once(st, cur_index, pb, out);

    if (st.duration_mcs == 0) {
      pb.uv = truncate_uv(vec2{pb.uv.x + st.uv.x, pb.uv.y + st.uv.y});
      if (st.next == invalid_state) {
        pb.finished = true;
        break;
      }
      if (++guard > zero_duration_step_limit) {
        utils::warn("flow: zero-duration transition limit {} reached at state {}", zero_duration_step_limit, cur_index);
        break;
      }
      enter_state(pb, st.next);
      continue;
    }

    const uint64_t before = pb.elapsed_mcs;
    const uint64_t step = std::min<uint64_t>(remaining, st.duration_mcs - std::min(before, st.duration_mcs));
    pb.elapsed_mcs = std::min<uint64_t>(st.duration_mcs, pb.elapsed_mcs + step);
    remaining -= step;

    const float prev_t = static_cast<float>(before) / static_cast<float>(st.duration_mcs);
    const float cur_t = static_cast<float>(pb.elapsed_mcs) / static_cast<float>(st.duration_mcs);
    const float delta_t = std::clamp(cur_t - prev_t, 0.0f, 1.0f);
    pb.uv = truncate_uv(vec2{pb.uv.x + st.uv.x * delta_t, pb.uv.y + st.uv.y * delta_t});

    const uint32_t image_count = static_cast<uint32_t>(st.images.size());
    if (image_count != 0) {
      const uint32_t img_index = directional_image_index(ctx.angle_rad, image_count);
      const image_ref& img = st.images[img_index];
      out.sprite = sprite_sample{img.image, img.mirror_state, pb.uv, img.image != nullptr};
    } else {
      out.sprite = sprite_sample{nullptr, mirror::none, pb.uv, false};
    }

    if (pb.elapsed_mcs < st.duration_mcs) break;

    if (st.next == invalid_state) {
      pb.finished = true;
      break;
    }

    enter_state(pb, st.next);
    if (remaining == 0) {
      const state& next_st = states_[pb.current].data;
      emit_action_once(next_st, pb.current, pb, out);
      break;
    }
  }

  return out;
}

uint8_t parse_mirror_suffix(const std::string_view token, std::string_view* without_suffix) {
  uint8_t mirror_state = mirror::none;
  std::string_view base = token;

  const size_t last_colon = find_last_colon(base);
  if (last_colon != std::string_view::npos) {
    const std::string_view suffix = base.substr(last_colon + 1);
    if (is_mirror_token(suffix)) {
      if (suffix.find('u') != std::string_view::npos) mirror_state |= mirror::u;
      if (suffix.find('v') != std::string_view::npos) mirror_state |= mirror::v;
      base = base.substr(0, last_colon);
    }
  }

  if (without_suffix != nullptr) *without_suffix = base;
  return mirror_state;
}

image_ref parse_image_ref(const std::string_view token, const demiurg::resource_system* resources) {
  std::string_view without_mirror;
  const uint8_t mirror_state = parse_mirror_suffix(token, &without_mirror);

  // `tex/img2:3:u` keeps selector `:3` as part of the resource query for now.
  // The atlas/sub-image contract lives outside flow; if demiurg does not know
  // selectors yet, fallback to the base id before the selector.
  const demiurg::resource_interface* res = nullptr;
  if (resources != nullptr && !without_mirror.empty()) {
    res = resources->get(without_mirror);
    if (res == nullptr) {
      const size_t selector_colon = find_last_colon(without_mirror);
      if (selector_colon != std::string_view::npos) {
        res = resources->get(without_mirror.substr(0, selector_colon));
      }
    }
  }

  if (res == nullptr && resources != nullptr) {
    utils::warn("flow: image resource '{}' was not found", token);
  }

  return image_ref{res, mirror_state};
}

uint32_t directional_image_index(const float angle_rad, const uint32_t image_count) noexcept {
  if (image_count <= 1) return 0;

  float a = std::fmod(angle_rad, tau_v);
  if (a < 0.0f) a += tau_v;

  const float sector = tau_v / static_cast<float>(image_count);
  return static_cast<uint32_t>(std::floor((a + sector * 0.5f) / sector)) % image_count;
}

vec2 truncate_uv(const vec2 value) noexcept {
  const auto trunc_one = [](float v) noexcept {
    v = std::fmod(v, 1.0f);
    if (v < 0.0f) v += 1.0f;
    return v;
  };

  return vec2{trunc_one(value.x), trunc_one(value.y)};
}

std::vector<state> parse_state_text(
  const std::string_view content,
  const std::string_view label,
  const demiurg::resource_system* resources,
  std::vector<std::string>* next_names,
  const parse_options options
) {
  tavl::parser p;
  p.add_default_operator();
  p.flush(std::string(content));
  p.finish();

  tavl::ct_context ctx;
  std::vector<state> states;
  std::vector<std::string> local_next;

  state_mirror val{};
  while (tavl::deserialize_next(p, ctx, val)) {
    local_next.push_back(val.next.value_or(std::string{}));
    states.push_back(convert_state(val, resources, label, states.size(), options));
    val = state_mirror{};
  }

  for (const auto& d : ctx.diagnostics) {
    if (!d.error.is_critical()) continue;
    const uint32_t line = d.error.span.line == 0 ? 0 : static_cast<uint32_t>(d.error.span.line) + options.line_offset;
    utils::warn("flow: could not parse '{}' as animation states: error '{}' at {}:{} field '{}'",
      label, tavl::to_string(d.error.type), line, d.error.span.column, d.field);
  }

  if (next_names != nullptr) *next_names = std::move(local_next);
  return states;
}

}
}
