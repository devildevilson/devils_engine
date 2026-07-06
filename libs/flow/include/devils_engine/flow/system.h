#ifndef DEVILS_ENGINE_FLOW_SYSTEM_H
#define DEVILS_ENGINE_FLOW_SYSTEM_H

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "devils_engine/demiurg/resource_system.h"
#include "devils_engine/utils/string_id.h"

namespace devils_engine {

namespace flow {

static constexpr uint32_t invalid_state = UINT32_MAX;
static constexpr uint32_t default_zero_duration_step_limit = 32;

namespace mirror {
enum values : uint8_t {
  none = 0,
  u = 1u << 0u,
  v = 1u << 1u,
  uv = u | v
};
}

struct vec2 {
  float x = 0.0f;
  float y = 0.0f;
};

struct image_ref {
  demiurg::resource_handle image;
  uint8_t mirror_state = mirror::none;
};

struct state {
  uint64_t duration_mcs = 0;
  uint32_t next = invalid_state;
  std::vector<image_ref> images;
  utils::id action = utils::invalid_id;
  vec2 uv = {};
};

struct playback {
  uint32_t current = invalid_state;
  uint64_t elapsed_mcs = 0;
  vec2 uv = {};
  bool action_emitted = false;
  bool finished = false;
};

struct sample_context {
  // Caller supplies the already-normalized presentation angle convention.
  // Bucket 0 is centered at angle 0; for 8 images it covers [-22.5, 22.5].
  float angle_rad = 0.0f;
};

struct sprite_sample {
  demiurg::resource_handle image;
  uint8_t mirror_state = mirror::none;
  vec2 uv = {};
  bool visible = false;
};

struct action_event {
  uint32_t state = invalid_state;
  utils::id action = utils::invalid_id;
};

struct sample_result {
  sprite_sample sprite;
  std::vector<action_event> actions;
};

struct parse_options {
  bool warn_on_odd_direction_count = true;
  uint32_t line_offset = 0;
};

class library {
public:
  uint32_t add_state(std::string name, state st);
  uint32_t find_state(std::string_view name) const noexcept;
  const state* get(uint32_t index) const noexcept;
  state* get(uint32_t index) noexcept;
  size_t size() const noexcept;

  bool set_next(uint32_t index, std::string_view next_name);
  uint32_t append_resource_states(
    std::string_view resource_id,
    const std::vector<state>& states,
    const std::vector<std::string>& next_names);
  void resolve_pending_links(bool warn_unresolved);

  sample_result sample(playback& pb, uint64_t dt_mcs, const sample_context& ctx,
                       uint32_t zero_duration_step_limit = default_zero_duration_step_limit) const;

private:
  struct named_state {
    std::string name;
    state data;
  };

  struct pending_link {
    uint32_t from = invalid_state;
    std::string next;
  };

  std::vector<named_state> states_;
  std::vector<pending_link> pending_;
};

uint8_t parse_mirror_suffix(std::string_view token, std::string_view* without_suffix = nullptr);
image_ref parse_image_ref(std::string_view token, const demiurg::resource_system* resources);
uint32_t directional_image_index(float angle_rad, uint32_t image_count) noexcept;
vec2 truncate_uv(vec2 value) noexcept;

std::vector<state> parse_state_text(
  std::string_view content,
  std::string_view label,
  const demiurg::resource_system* resources,
  std::vector<std::string>* next_names = nullptr,
  parse_options options = {});

}
}

#endif
