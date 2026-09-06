#include "devils_engine/aesthetics/sink.h"

#include <fstream>
#include <iterator>

#include "devils_engine/utils/core.h"

// World-only convenience wrappers. Generic checksum/compression/container work belongs to
// utils::serial::seal/unseal; this file only connects that envelope to the ECS projection.

namespace devils_engine::aesthetics::serial {

std::vector<std::byte> pack(const world* value, const sink_policy& policy,
                            const std::span<const uint8_t> screenshot) {
  return seal(dump_world(value), policy, screenshot);
}

bool unpack(const std::span<const std::byte> data, world* value,
            std::vector<uint8_t>* screenshot_output) {
  std::vector<std::byte> raw;
  std::vector<uint8_t> preview;
  if (!unseal(data, raw, screenshot_output != nullptr ? &preview : nullptr)) return false;
  reader input{raw};
  auto staged = stage_world(input);
  if (!staged.has_value() || !input.good() || input.position() != input.size()) return false;
  value->replace_state(std::move(*staged));
  if (screenshot_output != nullptr) *screenshot_output = std::move(preview);
  return true;
}

bool save_to_file(const world* value, const std::string& path, const sink_policy& policy,
                  const std::span<const uint8_t> screenshot) {
  const auto bytes = pack(value, policy, screenshot);
  std::ofstream file(path, std::ios::binary);
  if (!file) {
    utils::warn("snapshot: cannot open '{}' for write", path);
    return false;
  }
  file.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(file);
}

bool load_from_file(world* value, const std::string& path,
                    std::vector<uint8_t>* screenshot_output) {
  std::ifstream file(path, std::ios::binary);
  if (!file) {
    utils::warn("snapshot: cannot open '{}' for read", path);
    return false;
  }
  const std::vector<char> bytes(
    (std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
  return unpack(std::as_bytes(std::span{bytes}), value, screenshot_output);
}

} // namespace devils_engine::aesthetics::serial
