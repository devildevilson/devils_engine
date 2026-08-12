#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include <doctest/doctest.h>

#include "devils_engine/demiurg/module_interface.h"
#include "devils_engine/sound/common.h"
#include "devils_engine/sound/resource.h"
#include "devils_engine/sound/sound_resource.h"
#include "devils_engine/sound/system.h"
#include "devils_engine/utils/safe_handle.h"

using namespace devils_engine;

namespace {

void append_u16(std::vector<char>& out, const uint16_t value) {
  out.push_back(static_cast<char>(value & 0xffu));
  out.push_back(static_cast<char>((value >> 8u) & 0xffu));
}

void append_u32(std::vector<char>& out, const uint32_t value) {
  for (uint32_t shift = 0; shift < 32; shift += 8) {
    out.push_back(static_cast<char>((value >> shift) & 0xffu));
  }
}

std::vector<char> make_mono_wav() {
  constexpr uint32_t sample_rate = 8000;
  constexpr uint32_t sample_count = 16;
  constexpr uint32_t data_bytes = sample_count * sizeof(int16_t);
  std::vector<char> out;
  out.reserve(44 + data_bytes);
  const auto tag = [&out](const char* value) {
    out.insert(out.end(), value, value + 4);
  };
  tag("RIFF");
  append_u32(out, 36 + data_bytes);
  tag("WAVE");
  tag("fmt ");
  append_u32(out, 16);
  append_u16(out, 1); // PCM
  append_u16(out, 1); // mono
  append_u32(out, sample_rate);
  append_u32(out, sample_rate * sizeof(int16_t));
  append_u16(out, sizeof(int16_t));
  append_u16(out, 16);
  tag("data");
  append_u32(out, data_bytes);
  for (uint32_t i = 0; i < sample_count; ++i) {
    append_u16(out, static_cast<uint16_t>(i * 512));
  }
  return out;
}

class memory_sound_module final : public demiurg::module_interface {
public:
  explicit memory_sound_module(std::vector<char> bytes)
      : module_interface("memory/"), bytes_(std::move(bytes)) {}

  void open() override { opened_ = true; }
  void close() override { opened_ = false; }
  bool is_openned() const override { return opened_; }
  void resources_list(std::vector<demiurg::resource_candidate>&, uint32_t) const override {}
  void load_binary(const std::string&, std::vector<char>& mem) const override { mem = bytes_; }
  void load_binary(const std::string&, std::vector<uint8_t>& mem) const override {
    mem.resize(bytes_.size());
    std::memcpy(mem.data(), bytes_.data(), bytes_.size());
  }
  void load_text(const std::string&, std::string& mem) const override {
    mem.assign(bytes_.begin(), bytes_.end());
  }

private:
  std::vector<char> bytes_;
  bool opened_ = false;
};

} // namespace

TEST_CASE("sound format helpers convert samples, frames, bytes, and seconds consistently [sound::common]") {
  CHECK(sound::format_to_bytes(sound::format::u8) == 1);
  CHECK(sound::format_to_bytes(sound::format::s16) == 2);
  CHECK(sound::format_to_bytes(sound::format::s24) == 3);
  CHECK(sound::format_to_bytes(sound::format::s32) == 4);
  CHECK(sound::format_to_bytes(sound::format::f32) == 4);
  CHECK(sound::format_to_bytes(sound::format::unknown) == 0);

  CHECK(sound::bits_per_sample_to_format(8, false) == sound::format::u8);
  CHECK(sound::bits_per_sample_to_format(16, false) == sound::format::s16);
  CHECK(sound::bits_per_sample_to_format(24, false) == sound::format::s24);
  CHECK(sound::bits_per_sample_to_format(32, false) == sound::format::s32);
  CHECK(sound::bits_per_sample_to_format(32, true) == sound::format::f32);
  CHECK(sound::bits_per_sample_to_format(12, false) == sound::format::unknown);

  constexpr std::size_t sample_rate = 48000;
  constexpr std::uint32_t channels = 2;
  constexpr auto format = sound::format::f32;

  CHECK(sound::pcm_frame_to_bytes(channels, format) == 8);
  CHECK(sound::pcm_samples_to_bytes(sample_rate, channels, format) == sample_rate * channels * sizeof(float));
  CHECK(sound::seconds_to_bytes(0.5, sample_rate, channels, format) == sample_rate * channels * sizeof(float) / 2);
  CHECK(sound::bytes_to_pcm_frames(sample_rate * channels * sizeof(float), channels, format) == sample_rate);
  CHECK(sound::bytes_to_seconds(sample_rate * channels * sizeof(float), sample_rate, channels, format) == doctest::Approx(1.0));
}

TEST_CASE("sound vec3 helpers compute distance and normalization [sound::common]") {
  const sound::vec3 a(1.0f, 2.0f, 3.0f);
  const sound::vec3 b(4.0f, 6.0f, 3.0f);

  const sound::vec3 diff = b - a;
  CHECK(diff.x == doctest::Approx(3.0f));
  CHECK(diff.y == doctest::Approx(4.0f));
  CHECK(diff.z == doctest::Approx(0.0f));
  CHECK(sound::dot2(diff, diff) == doctest::Approx(25.0f));
  CHECK(sound::distance2(a, b) == doctest::Approx(25.0f));

  const sound::vec3 n = sound::normalize(diff);
  CHECK(n.x == doctest::Approx(0.6f));
  CHECK(n.y == doctest::Approx(0.8f));
  CHECK(n.z == doctest::Approx(0.0f));
}

TEST_CASE("sound task defaults are valid for a new positional sound request [sound::system2]") {
  sound::task task;

  CHECK(task.id == SIZE_MAX);
  CHECK(task.res.id.empty());
  CHECK(task.res.type == sound::data_type::undefined);
  CHECK(task.res.data.empty());
  CHECK(task.command == sound::task::command::play);
  CHECK(task.type == sound::type::sfx);
  CHECK(task.pitch == doctest::Approx(1.0f));
  CHECK(task.volume == doctest::Approx(1.0f));
  CHECK(task.start == doctest::Approx(0.0));
  CHECK(task.after == SIZE_MAX);
}

TEST_CASE("sound task stores resource view without taking ownership [sound::system2]") {
  std::string id = "test/sound";
  std::vector<char> bytes = {'R', 'I', 'F', 'F'};

  sound::resource2 res;
  res.id = id;
  res.type = sound::data_type::wav;
  res.data = std::span<const char>(bytes.data(), bytes.size());

  const sound::task task(42, res);
  CHECK(task.id == 42);
  CHECK(task.res.id == id);
  CHECK(task.res.type == sound::data_type::wav);
  CHECK(task.res.data.data() == bytes.data());
  CHECK(task.res.data.size() == bytes.size());
}

TEST_CASE("sound resource pins queued and active task data across unload [sound::lifetime]") {
  memory_sound_module module(make_mono_wav());
  sound::sound_resource resource;
  resource.module = &module;
  resource.set("tone.wav", "fixture", "sounds/tone", "wav");
  resource.load_cold(utils::safe_handle_t{});

  std::weak_ptr<const sound::resource_blob> lifetime;
  {
    const auto queued = resource.view();
    REQUIRE(queued.pinned());
    REQUIRE_FALSE(queued.data.empty());
    CHECK(queued.id == "sounds/tone");
    const auto* const bytes = queued.data.data();
    lifetime = queued.owner;

    {
      sound::task active(73, queued);
      resource.unload_warm(utils::safe_handle_t{});

      CHECK_FALSE(resource.view().pinned());
      CHECK(active.res.pinned());
      CHECK(active.res.data.data() == bytes);
      CHECK_FALSE(active.res.data.empty());
      CHECK_FALSE(lifetime.expired());
    }

    CHECK_FALSE(lifetime.expired()); // queued command still owns the immutable generation
  }

  CHECK(lifetime.expired()); // last command/task pin released the generation
}

TEST_CASE("sound system can enumerate playback devices without requiring one to exist [sound::system2]") {
  std::vector<std::string> devices;
  const bool enumerated = sound::system2::playback_devices(devices);

  if (!enumerated) {
    MESSAGE("miniaudio could not enumerate playback devices in this environment");
    CHECK(devices.empty());
    return;
  }

  for (const auto& name : devices) {
    CHECK_FALSE(name.empty());
  }
}

TEST_CASE("sound system smoke constructs on an enumerated or default playback device when available [sound::system2]") {
  std::vector<std::string> devices;
  if (!sound::system2::playback_devices(devices) || devices.empty()) {
    MESSAGE("no playback device available for system2 construction smoke test");
    return;
  }

  CHECK_NOTHROW(sound::system2(devices.front(), 0.1, 512));
}
