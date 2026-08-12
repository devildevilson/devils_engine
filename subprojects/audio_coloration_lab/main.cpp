#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include <devils_engine/sound/resource.h>
#include <devils_engine/sound/system.h>

namespace {

using devils_engine::sound::vec3;
using steady_clock = std::chrono::steady_clock;

constexpr uint32_t sample_rate = 48000;
constexpr double scenario_duration = 20.0;
constexpr double pi = 3.14159265358979323846;

enum class signal_kind { hum,
                         noise };

struct options {
  bool coloration = true;
  bool dry_run = false;
  bool list_devices = false;
  std::string device;
  double duration = scenario_duration;
  float gain = 0.35f;
  float strength = 1.0f;
  signal_kind signal = signal_kind::hum;
};

[[noreturn]] void usage_error(const std::string_view message) {
  throw std::runtime_error(std::string(message) + "\nUse --help for command syntax.");
}

void print_help() {
  std::cout
    << "audio_coloration_lab - production miniaudio directional-coloration A/B\n\n"
    << "  --coloration on|off   enable the bounded high-shelf cue (default on)\n"
    << "  --device TEXT         exact playback device name\n"
    << "  --duration SECONDS    playback duration (default 20)\n"
    << "  --gain VALUE          master gain [0,1] (default 0.35)\n"
    << "  --strength VALUE      coloration multiplier [0,2] (default 1)\n"
    << "  --signal hum|noise    harmonic continuous hum or white noise (default hum)\n"
    << "  --list-devices        print miniaudio playback devices and exit\n"
    << "  --dry-run             validate trajectory/response without a device\n"
    << "  --help                show this text\n";
}

options parse_options(const int argc, char** argv) {
  options out;
  const auto next = [&](int& i, const std::string_view flag) -> std::string_view {
    if (++i >= argc) usage_error(std::string(flag) + " requires a value");
    return argv[i];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--coloration") {
      const auto value = next(i, arg);
      if (value == "on") out.coloration = true;
      else if (value == "off") out.coloration = false;
      else usage_error("coloration must be on or off");
    } else if (arg == "--device") {
      out.device = next(i, arg);
    } else if (arg == "--duration") {
      out.duration = std::stod(std::string(next(i, arg)));
    } else if (arg == "--gain") {
      out.gain = std::stof(std::string(next(i, arg)));
    } else if (arg == "--strength") {
      out.strength = std::stof(std::string(next(i, arg)));
    } else if (arg == "--signal") {
      const auto value = next(i, arg);
      if (value == "hum") out.signal = signal_kind::hum;
      else if (value == "noise") out.signal = signal_kind::noise;
      else usage_error("signal must be hum or noise");
    } else if (arg == "--list-devices") {
      out.list_devices = true;
    } else if (arg == "--dry-run") {
      out.dry_run = true;
    } else {
      usage_error(std::string("unknown argument: ") + std::string(arg));
    }
  }

  if (!std::isfinite(out.duration) || out.duration <= 0.0) usage_error("duration must be positive");
  if (!std::isfinite(out.gain) || out.gain < 0.0f || out.gain > 1.0f) usage_error("gain must be in [0,1]");
  if (!std::isfinite(out.strength) || out.strength < 0.0f || out.strength > 2.0f) usage_error("strength must be in [0,2]");
  return out;
}

vec3 trajectory(const double seconds) {
  const double t = std::fmod(std::max(seconds, 0.0), scenario_duration);
  if (t < 8.0) {
    const double angle = 2.0 * pi * t / 8.0;
    return vec3(static_cast<float>(4.0 * std::sin(angle)), 0.0f,
                static_cast<float>(-4.0 * std::cos(angle)));
  }
  if (t < 16.0) {
    const double angle = 2.0 * pi * (t - 8.0) / 8.0;
    return vec3(0.0f, static_cast<float>(4.0 * std::sin(angle)),
                static_cast<float>(-4.0 * std::cos(angle)));
  }

  // Isolated elevation cue: above/below have identical radius, stereo panning and distance gain.
  // Only the optional coloration node can distinguish these one-second holds.
  const bool above = static_cast<int>(t - 16.0) % 2 == 0;
  return vec3(0.0f, above ? 4.0f : -4.0f, 0.0f);
}

std::string_view phase_name(const double seconds) {
  const double t = std::fmod(std::max(seconds, 0.0), scenario_duration);
  if (t < 8.0) return "horizontal orbit";
  if (t < 16.0) return "vertical orbit";
  return static_cast<int>(t - 16.0) % 2 == 0 ? "ABOVE hold" : "BELOW hold";
}

float radius(const vec3& pos) {
  return std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
}

std::string_view signal_name(const signal_kind signal) noexcept {
  return signal == signal_kind::hum ? "harmonic hum" : "white noise";
}

std::vector<int16_t> reference_signal(const double duration, const signal_kind kind) {
  const size_t count = static_cast<size_t>(std::ceil(duration * sample_rate));
  std::vector<int16_t> out(count);
  uint32_t rng = 0x9e3779b9u;
  const size_t fade_frames = std::min<size_t>(sample_rate / 50u, count / 2u); // 20 ms
  for (size_t i = 0; i < count; ++i) {
    float envelope = 1.0f;
    if (fade_frames > 0) {
      const float fade_in = static_cast<float>(i) / static_cast<float>(fade_frames);
      const float fade_out = static_cast<float>(count - 1u - i) / static_cast<float>(fade_frames);
      envelope = std::clamp(std::min(fade_in, fade_out), 0.0f, 1.0f);
    }
    float sample = 0.0f;
    if (kind == signal_kind::noise) {
      rng ^= rng << 13u;
      rng ^= rng >> 17u;
      rng ^= rng << 5u;
      const float noise = static_cast<float>(rng >> 8u) / 8388607.5f - 1.0f;
      sample = 0.16f * noise;
    } else {
      // A phase-continuous 110 Hz buzz with harmonics through 7.04 kHz. Unlike a pure sine it has
      // stable energy on both sides of the 2.5 kHz shelf, so changes read as timbre, not just gain.
      const double time = static_cast<double>(i) / sample_rate;
      for (uint32_t harmonic = 1; harmonic <= 64; ++harmonic) {
        sample += 0.16f / static_cast<float>(harmonic) *
                  static_cast<float>(std::sin(2.0 * pi * 110.0 * harmonic * time));
      }
    }
    sample *= envelope;
    out[i] = static_cast<int16_t>(std::lrint(std::clamp(sample, -0.9f, 0.9f) * std::numeric_limits<int16_t>::max()));
  }
  return out;
}

uint64_t signal_hash(const std::vector<int16_t>& pcm) {
  uint64_t hash = 14695981039346656037ull;
  const auto* bytes = reinterpret_cast<const uint8_t*>(pcm.data());
  for (size_t i = 0; i < pcm.size() * sizeof(pcm[0]); ++i) {
    hash ^= bytes[i];
    hash *= 1099511628211ull;
  }
  return hash;
}

devils_engine::sound::directional_coloration_config coloration_config(const options& opts) {
  devils_engine::sound::directional_coloration_config config;
  config.enabled = opts.coloration;
  config.strength = opts.strength;
  return config;
}

void print_sample(const double seconds, const options& opts) {
  const vec3 pos = trajectory(seconds);
  const auto response = devils_engine::sound::compute_directional_coloration(
    coloration_config(opts), vec3{}, vec3(0.0f, 0.0f, -1.0f),
    vec3(0.0f, 1.0f, 0.0f), pos);
  std::cout << std::fixed << std::setprecision(2)
            << "t=" << seconds << " " << std::setw(18) << phase_name(seconds)
            << " pos=(" << pos.x << ", " << pos.y << ", " << pos.z << ")"
            << " r=" << radius(pos) << " shelf=" << response.high_shelf_db << " dB\n";
}

void validate_trajectory() {
  for (double t = 0.0; t < scenario_duration; t += 0.125) {
    if (std::abs(radius(trajectory(t)) - 4.0f) > 0.0001f) {
      throw std::runtime_error("constant-radius trajectory invariant failed");
    }
  }
}

void print_header(const options& opts, const std::vector<int16_t>& pcm) {
  std::cout << "Reference PCM: " << signal_name(opts.signal) << ", mono s16, "
            << sample_rate << " Hz, FNV64=0x"
            << std::hex << signal_hash(pcm) << std::dec << '\n'
            << "Listener: origin, forward -Z, up +Y; source radius=4\n"
            << "Coloration: " << (opts.coloration ? "ON" : "OFF")
            << " (strength " << opts.strength
            << ", behind -2.25 dB, above +0.65 dB, below -0.85 dB, shelf 2.5 kHz)\n"
            << "Phases: 0-8s horizontal orbit; 8-16s vertical orbit; "
               "16-20s isolated ABOVE/BELOW one-second holds\n";
}

void run_audio(const options& opts, const std::vector<int16_t>& pcm) {
  devils_engine::sound::system engine(opts.device, 1.0, sample_rate);
  engine.set_master_volume(opts.gain);
  engine.set_directional_coloration(coloration_config(opts));
  engine.set_listener_pos(vec3{});
  engine.set_listener_ori(vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));

  auto blob = std::make_shared<devils_engine::sound::resource_blob>();
  blob->id = "audio_coloration_lab/reference";
  blob->type = devils_engine::sound::data_type::pcm;
  blob->sample_format = devils_engine::sound::format::s16;
  blob->channels = 1;
  blob->sample_rate = sample_rate;
  blob->frames_count = pcm.size();
  blob->data.resize(pcm.size() * sizeof(pcm[0]));
  std::memcpy(blob->data.data(), pcm.data(), blob->data.size());

  devils_engine::sound::task task(1, devils_engine::sound::resource2(blob));
  task.type = devils_engine::sound::type::sfx;
  task.pos = trajectory(0.0);
  task.min_distance = 1.0f;
  task.max_distance = 12.0f;
  if (!engine.setup_sound(task)) throw std::runtime_error("sound system rejected reference task");

  std::cout << "Device: " << engine.playback_device_name() << ", "
            << engine.playback_rate() << " Hz, " << engine.playback_channel_count() << " channels\n";
  const auto begin = steady_clock::now();
  auto previous = begin;
  int printed_second = -1;
  for (;;) {
    const auto now = steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - begin).count();
    if (elapsed >= opts.duration) break;
    const double delta = std::chrono::duration<double>(now - previous).count();
    previous = now;
    engine.update(static_cast<size_t>(std::max(1.0, delta * 1'000'000.0)));
    devils_engine::sound::task_update update{};
    update.id = task.id;
    update.pos = trajectory(elapsed);
    engine.update_sound(update);
    const int second = static_cast<int>(elapsed);
    if (second != printed_second) {
      printed_second = second;
      print_sample(elapsed, opts);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    const options opts = parse_options(argc, argv);
    if (opts.list_devices) {
      std::vector<std::string> devices;
      devils_engine::sound::system::playback_devices(devices);
      for (const auto& device : devices) std::cout << device << '\n';
      return 0;
    }

    validate_trajectory();
    const auto pcm = reference_signal(opts.duration, opts.signal);
    print_header(opts, pcm);
    if (opts.dry_run) {
      for (double t = 0.0; t < std::min(opts.duration, scenario_duration); t += 1.0) print_sample(t, opts);
      return 0;
    }
    run_audio(opts, pcm);
    std::cout << "Pass complete. Repeat with --coloration " << (opts.coloration ? "off" : "on") << ".\n";
    return 0;
  } catch (const std::exception& error) {
    std::cerr << "audio_coloration_lab: " << error.what() << '\n';
    return 1;
  }
}
