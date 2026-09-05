#ifndef DEVILS_ENGINE_NET06_CAUSAL_FIXTURE_H
#define DEVILS_ENGINE_NET06_CAUSAL_FIXTURE_H

#include <bit>
#include <cstddef>
#include <vector>

#include <devils_engine/network/network.h>

namespace net06_fixture {
namespace net = devils_engine::network;

struct causal_state {
  float x = 0, velocity = 0;
  unsigned tick = 0;
};

struct causal_host {
  using staging_type = causal_state;
  causal_state state;
  unsigned presentation = 0;
};

struct causal_section {
  static constexpr std::uint32_t id = 1, version = 1;

  static void write(const causal_host& host, net::state_writer& writer) {
    writer.u32(std::bit_cast<std::uint32_t>(host.state.x));
    writer.u32(std::bit_cast<std::uint32_t>(host.state.velocity));
    writer.u32(host.state.tick);
  }

  static bool read(causal_state& state, net::state_reader& reader) {
    state.x = std::bit_cast<float>(reader.u32());
    state.velocity = std::bit_cast<float>(reader.u32());
    state.tick = reader.u32();
    return reader.good();
  }

  static bool validate(const causal_state&) {
    return true;
  }
};

using causal_schema = net::state_schema<
  causal_host, net::state_writer, net::state_reader, causal_section>;

struct late_intent {
  unsigned tick;
  float velocity;
};

struct intent_size {
  std::size_t operator()(const late_intent&) const noexcept {
    return 8;
  }
};

struct blob_size {
  std::size_t operator()(const std::vector<std::byte>& bytes) const noexcept {
    return bytes.size();
  }
};

} // namespace net06_fixture

#endif
