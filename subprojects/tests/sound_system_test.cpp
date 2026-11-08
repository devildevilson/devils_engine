#include <algorithm>
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
  const sound::vec3 zero = sound::normalize(sound::vec3{});
  CHECK(zero.x == doctest::Approx(0.0f));
  CHECK(zero.y == doctest::Approx(0.0f));
  CHECK(zero.z == doctest::Approx(0.0f));
}

TEST_CASE("directional coloration is bounded, listener-relative, and disabled by default [sound::coloration]") {
  sound::directional_coloration_config config;
  const sound::vec3 listener{};
  const sound::vec3 forward(0.0f, 0.0f, -1.0f);
  const sound::vec3 up(0.0f, 1.0f, 0.0f);

  CHECK(sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, 0.0f, -4.0f)).high_shelf_db == doctest::Approx(0.0f));

  config.enabled = true;
  const auto front = sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, 0.0f, -4.0f));
  const auto behind = sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, 0.0f, 4.0f));
  const auto above = sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, 4.0f, 0.0f));
  const auto below = sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, -4.0f, 0.0f));

  CHECK(front.high_shelf_db == doctest::Approx(0.0f));
  CHECK(behind.behind == doctest::Approx(1.0f));
  CHECK(behind.high_shelf_db == doctest::Approx(config.behind_high_shelf_db));
  CHECK(above.above == doctest::Approx(1.0f));
  CHECK(above.high_shelf_db == doctest::Approx(config.above_high_shelf_db));
  CHECK(below.below == doctest::Approx(1.0f));
  CHECK(below.high_shelf_db == doctest::Approx(config.below_high_shelf_db));

  config.strength = 0.5f;
  const auto half_behind = sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, 0.0f, 4.0f));
  CHECK(half_behind.high_shelf_db == doctest::Approx(config.behind_high_shelf_db * 0.5f));
  config.strength = 0.0f;
  const auto neutral_behind = sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, 0.0f, 4.0f));
  CHECK(neutral_behind.high_shelf_db == doctest::Approx(0.0f));
  config.strength = 1.0f;

  // Constant radius cannot affect the response; only listener-relative direction can.
  const auto far_behind = sound::compute_directional_coloration(config, listener, forward, up, sound::vec3(0.0f, 0.0f, 400.0f));
  CHECK(far_behind.high_shelf_db == doctest::Approx(behind.high_shelf_db));

  config.behind_high_shelf_db = -100.0f;
  config.above_high_shelf_db = 100.0f;
  config.below_high_shelf_db = -100.0f;
  config.strength = 100.0f;
  config.high_shelf_frequency_hz = 100000.0f;
  config.smoothing_ms = 0.0f;
  const auto sanitized = sound::sanitize_directional_coloration_config(config);
  CHECK(sanitized.behind_high_shelf_db == doctest::Approx(-3.0f));
  CHECK(sanitized.above_high_shelf_db == doctest::Approx(1.0f));
  CHECK(sanitized.below_high_shelf_db == doctest::Approx(-1.0f));
  CHECK(sanitized.strength == doctest::Approx(2.0f));
  CHECK(sanitized.high_shelf_frequency_hz == doctest::Approx(12000.0f));
  CHECK(sanitized.smoothing_ms == doctest::Approx(10.0f));
}

TEST_CASE("sound task defaults are valid for a new positional sound request [sound::system]") {
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

TEST_CASE("sound task stores resource view without taking ownership [sound::system]") {
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

TEST_CASE("sound system can enumerate playback devices without requiring one to exist [sound::system]") {
  std::vector<std::string> devices;
  const bool enumerated = sound::system::playback_devices(devices);

  if (!enumerated) {
    MESSAGE("miniaudio could not enumerate playback devices in this environment");
    CHECK(devices.empty());
    return;
  }

  for (const auto& name : devices) {
    CHECK_FALSE(name.empty());
  }
}

TEST_CASE("sound system smoke constructs on an enumerated or default playback device when available [sound::system]") {
  std::vector<std::string> devices;
  if (!sound::system::playback_devices(devices) || devices.empty()) {
    MESSAGE("no playback device available for system construction smoke test");
    return;
  }

  for (auto mixing : {sound::mixing_mode::owner, sound::mixing_mode::device}) {
    sound::system_config config;
    config.device_name = devices.front();
    config.stream_buffer_seconds = .1;
    config.decode_frames_per_update = 512;
    config.mixing = mixing;
    sound::system audio(config);
    CHECK(audio.native_device_count() == 1);
    CHECK(audio.resource_manager_worker_count() == 0);
    CHECK(audio.playback_rate() > 0);
    CHECK(audio.playback_channel_count() > 0);
    CHECK(audio.mixing() == mixing);
  }
}

TEST_CASE("offline audio owns no device or resource-manager worker and renders decoded sound") {
  for (auto mixing : {sound::mixing_mode::owner, sound::mixing_mode::device})
  for (const uint32_t rate : {44100u, 48000u, 96000u}) {
    sound::system_config config;
    config.offline = true;
    config.mixing = mixing;
    config.sample_rate = rate;
    config.stream_buffer_seconds = 0.05;
    sound::system audio(config);
    REQUIRE(audio.offline());
    CHECK(audio.playback_rate() == rate);
    CHECK(audio.playback_channel_count() == 2);
    CHECK(audio.resource_manager_worker_count() == 0);
    CHECK(audio.native_device_count() == 0);
    auto blob = std::make_shared<sound::resource_blob>();
    blob->id = "offline/test";
    blob->type = sound::data_type::wav;
    blob->data = make_mono_wav();
    sound::task task(7, sound::resource2(blob));
    task.type = sound::type::sfx;
    task.max_distance = 0;
    REQUIRE(audio.setup_sound(task));
    audio.update(0);
    std::vector<float> samples(rate / 100 * 2);
    audio.render(samples);
    CHECK(std::any_of(samples.begin(), samples.end(), [](float value) { return std::abs(value) > 0.00001f; }));
    for (size_t i = 0; i < 150; ++i) {
      audio.update(10000);
      audio.render(samples);
    }
    sound::task_status status{};
    CHECK_FALSE(audio.stat_sound(7, status));
  }
}

TEST_CASE("owner mixing matches direct mixing through wraparound and output underflow is silence") {
  sound::system_config cfg;
  cfg.offline = true;
  cfg.mixing = sound::mixing_mode::owner;
  cfg.output_buffer_seconds = .01;
  cfg.stream_buffer_seconds = .1;
  sound::system owner(cfg);
  cfg.mixing = sound::mixing_mode::device;
  sound::system direct(cfg);
  auto blob = std::make_shared<sound::resource_blob>();
  blob->id = "mix/compare";
  blob->type = sound::data_type::pcm;
  blob->sample_format = sound::format::f32;
  blob->sample_rate = 48000;
  blob->channels = 1;
  blob->frames_count = 48000;
  blob->data.resize(48000 * sizeof(float));
  for (size_t i = 0; i < blob->frames_count; ++i) {
    const float value = .1f * std::sin(double(i) * .031);
    std::memcpy(blob->data.data() + i * sizeof(float), &value, sizeof(float));
  }
  sound::task task(1, sound::resource2(blob));
  task.type = sound::type::sfx;
  task.max_distance = 0;
  REQUIRE(owner.setup_sound(task));
  REQUIRE(direct.setup_sound(task));
  size_t mismatches = 0;
  for (size_t i = 0; i < 100; ++i) {
    owner.update(1000);
    direct.update(1000);
    REQUIRE(owner.queued_output_frames() == 480);
    const size_t frames = i % 2 ? 97 : 181;
    std::vector<float> a(frames * 2), b(a.size());
    owner.render(a);
    direct.render(b);
    for (size_t j = 0; j < a.size(); ++j) mismatches += std::abs(a[j] - b[j]) > 1e-6f;
    CHECK(owner.queued_output_frames() == 480 - frames);
    CHECK(direct.queued_output_frames() == 0);
  }
  CHECK(mismatches == 0);
  std::vector<float> excess(2000, 1);
  const size_t queued = owner.queued_output_frames();
  owner.render(excess);
  CHECK(owner.queued_output_frames() == 0);
  CHECK(std::all_of(excess.begin() + queued * 2, excess.end(), [](float value) { return value == 0; }));
  owner.update(1000);
  CHECK(owner.queued_output_frames() == 480);
  CHECK(sound::parse_mixing_mode("owner") == sound::mixing_mode::owner);
  CHECK(sound::parse_mixing_mode("device") == sound::mixing_mode::device);
  CHECK_THROWS_AS(sound::parse_mixing_mode("automatic"), utils::critical_error);
  cfg.mixing = sound::mixing_mode::owner;
  cfg.output_buffer_seconds = 0;
  CHECK_THROWS_AS(sound::system{cfg}, utils::critical_error);
  cfg.mixing = sound::mixing_mode::device; // buffer setting is unused here.
  CHECK_NOTHROW(sound::system{cfg});
}
