#include <doctest/doctest.h>

#include <cmath>
#include <cstddef>
#include <span>
#include <string>
#include <vector>

#include "devils_engine/sound/common.h"
#include "devils_engine/sound/resource.h"
#include "devils_engine/sound/system.h"

using namespace devils_engine;

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
