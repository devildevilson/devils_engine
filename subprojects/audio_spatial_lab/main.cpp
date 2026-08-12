#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
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

#include <AL/al.h>
#include <AL/alc.h>
#include <AL/alext.h>

#include <devils_engine/sound/resource.h>
#include <devils_engine/sound/system.h>
#include <devils_engine/utils/core.h>

namespace {

using devils_engine::sound::vec3;
using steady_clock = std::chrono::steady_clock;

constexpr uint32_t sample_rate = 48000;
constexpr double scenario_duration = 28.0;
constexpr double pi = 3.14159265358979323846;

enum class backend_kind { miniaudio, openal };
enum class hrtf_mode { automatic, enabled, disabled };

struct options {
  backend_kind backend = backend_kind::miniaudio;
  hrtf_mode hrtf = hrtf_mode::automatic;
  std::string device;
  double duration = scenario_duration;
  float gain = 0.35f;
  float min_distance = 1.0f;
  float max_distance = 12.0f;
  bool list_devices = false;
  bool dry_run = false;
};

[[noreturn]] void usage_error(const std::string_view message) {
  throw std::runtime_error(std::string(message) + "\nUse --help for command syntax.");
}

void print_help() {
  std::cout
    << "audio_spatial_lab - matched miniaudio/OpenAL 3D A/B trajectory\n\n"
    << "  --backend miniaudio|openal   backend for this pass (default miniaudio)\n"
    << "  --device TEXT                case-insensitive device-name substring\n"
    << "  --hrtf auto|on|off           OpenAL Soft HRTF request (default auto)\n"
    << "  --duration SECONDS           playback duration (default 28)\n"
    << "  --gain VALUE                 listener/master gain [0,1] (default 0.35)\n"
    << "  --min-distance VALUE         full-gain distance (default 1)\n"
    << "  --max-distance VALUE         silence/clamp edge (default 12)\n"
    << "  --list-devices               print both backend device lists and exit\n"
    << "  --dry-run                    validate signal/trajectory without audio device\n"
    << "  --help                       show this text\n";
}

options parse_options(const int argc, char** argv) {
  options out;
  const auto next = [&](int& i, const std::string_view flag) -> std::string_view {
    if (++i >= argc) {
      usage_error(std::string(flag) + " requires a value");
    }
    return argv[i];
  };

  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--help") {
      print_help();
      std::exit(0);
    } else if (arg == "--backend") {
      const auto value = next(i, arg);
      if (value == "miniaudio") out.backend = backend_kind::miniaudio;
      else if (value == "openal") out.backend = backend_kind::openal;
      else usage_error("unknown backend");
    } else if (arg == "--device") {
      out.device = next(i, arg);
    } else if (arg == "--hrtf") {
      const auto value = next(i, arg);
      if (value == "auto") out.hrtf = hrtf_mode::automatic;
      else if (value == "on") out.hrtf = hrtf_mode::enabled;
      else if (value == "off") out.hrtf = hrtf_mode::disabled;
      else usage_error("unknown HRTF mode");
    } else if (arg == "--duration") {
      out.duration = std::stod(std::string(next(i, arg)));
    } else if (arg == "--gain") {
      out.gain = std::stof(std::string(next(i, arg)));
    } else if (arg == "--min-distance") {
      out.min_distance = std::stof(std::string(next(i, arg)));
    } else if (arg == "--max-distance") {
      out.max_distance = std::stof(std::string(next(i, arg)));
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
  if (!std::isfinite(out.min_distance) || out.min_distance < 0.0f) usage_error("min distance must be non-negative");
  if (!std::isfinite(out.max_distance) || out.max_distance <= out.min_distance) {
    usage_error("max distance must be greater than min distance");
  }
  return out;
}

std::string lowercase(std::string value) {
  std::transform(value.begin(), value.end(), value.begin(), [](const unsigned char c) {
    return static_cast<char>(std::tolower(c));
  });
  return value;
}

std::string select_device(const std::vector<std::string>& devices, const std::string& query) {
  if (query.empty()) return {};
  const auto needle = lowercase(query);
  const auto it = std::find_if(devices.begin(), devices.end(), [&](const std::string& value) {
    return lowercase(value).find(needle) != std::string::npos;
  });
  if (it == devices.end()) {
    throw std::runtime_error("no playback device contains '" + query + "'");
  }
  return *it;
}

std::vector<std::string> openal_devices() {
  std::vector<std::string> out;
  ALCenum token = ALC_DEVICE_SPECIFIER;
  if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATE_ALL_EXT") == ALC_TRUE) {
    token = ALC_ALL_DEVICES_SPECIFIER;
  } else if (alcIsExtensionPresent(nullptr, "ALC_ENUMERATION_EXT") != ALC_TRUE) {
    return out;
  }

  const ALCchar* names = alcGetString(nullptr, token);
  while (names != nullptr && *names != '\0') {
    out.emplace_back(names);
    names += out.back().size() + 1;
  }
  return out;
}

void print_devices() {
  std::vector<std::string> mini;
  devils_engine::sound::system2::playback_devices(mini);
  std::cout << "miniaudio devices:\n";
  for (const auto& name : mini) std::cout << "  " << name << '\n';
  if (mini.empty()) std::cout << "  <none reported>\n";

  const auto openal = openal_devices();
  std::cout << "OpenAL devices:\n";
  for (const auto& name : openal) std::cout << "  " << name << '\n';
  if (openal.empty()) std::cout << "  <none reported>\n";
}

float cosine_lerp(const float a, const float b, const double local) {
  const double t = std::clamp(local, 0.0, 1.0);
  const double smooth = 0.5 - 0.5 * std::cos(pi * t);
  return static_cast<float>(a + (b - a) * smooth);
}

float distance_pulse(const double local) {
  if (local < 2.0) return cosine_lerp(4.0f, 10.0f, local / 2.0);
  if (local < 4.0) return cosine_lerp(10.0f, 1.0f, (local - 2.0) / 2.0);
  return cosine_lerp(1.0f, 4.0f, (local - 4.0) / 2.0);
}

vec3 trajectory(const double seconds) {
  const double t = std::fmod(std::max(0.0, seconds), scenario_duration);
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

  if (t < 22.0) {
    return vec3(0.0f, 0.0f, -distance_pulse(t - 16.0));
  }
  return vec3(0.0f, distance_pulse(t - 22.0), 0.0f);
}

std::string_view phase_name(const double seconds) {
  const double t = std::fmod(std::max(0.0, seconds), scenario_duration);
  if (t < 8.0) return "horizontal orbit";
  if (t < 16.0) return "vertical orbit";
  if (t < 22.0) return "front distance pulse";
  return "up distance pulse";
}

float source_distance(const vec3& pos) {
  return std::sqrt(pos.x * pos.x + pos.y * pos.y + pos.z * pos.z);
}

void validate_trajectory() {
  constexpr float epsilon = 0.0001f;
  for (double t = 0.0; t < 16.0; t += 0.125) {
    if (std::abs(source_distance(trajectory(t)) - 4.0f) > epsilon) {
      throw std::runtime_error("orbit radius invariant failed");
    }
  }

  for (double local = 0.0; local < 6.0; local += 0.125) {
    const auto front = trajectory(16.0 + local);
    const auto up = trajectory(22.0 + local);
    if (std::abs(source_distance(front) - source_distance(up)) > epsilon ||
        std::abs(front.x) > epsilon || std::abs(front.y) > epsilon || front.z > 0.0f ||
        std::abs(up.x) > epsilon || std::abs(up.z) > epsilon || up.y < 0.0f) {
      throw std::runtime_error("front/up distance-pulse parity invariant failed");
    }
  }
}

std::vector<int16_t> reference_signal(const double duration) {
  const size_t count = static_cast<size_t>(std::ceil((duration + 0.5) * sample_rate));
  std::vector<int16_t> out(count);
  uint32_t rng = 0x9e3779b9u;
  for (size_t i = 0; i < count; ++i) {
    rng ^= rng << 13u;
    rng ^= rng >> 17u;
    rng ^= rng << 5u;
    const float noise = (static_cast<float>(rng >> 8u) / 8388607.5f) - 1.0f;
    const double t = static_cast<double>(i) / sample_rate;
    float sample = 0.10f * noise;
    sample += 0.08f * static_cast<float>(std::sin(2.0 * pi * 220.0 * t));
    sample += 0.05f * static_cast<float>(std::sin(2.0 * pi * 880.0 * t));
    const size_t click_frame = i % (sample_rate / 2u);
    if (click_frame < 96u) {
      sample += 0.35f * std::exp(-static_cast<float>(click_frame) / 18.0f);
    }
    sample = std::clamp(sample, -0.9f, 0.9f);
    out[i] = static_cast<int16_t>(std::lrint(sample * std::numeric_limits<int16_t>::max()));
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

void print_run_header(const options& opts, const std::vector<int16_t>& pcm) {
  std::cout << "Reference PCM: mono s16, " << sample_rate << " Hz, " << pcm.size()
            << " frames, FNV64=0x" << std::hex << signal_hash(pcm) << std::dec << '\n'
            << "Listener: origin, forward -Z, up +Y; Doppler disabled/zero velocity\n"
            << "Attenuation: linear clamped, min=" << opts.min_distance
            << ", max=" << opts.max_distance << ", gain=" << opts.gain << '\n'
            << "Trajectory: 0-8s horizontal orbit; 8-16s vertical orbit (radius=4); "
               "16-22s front distance pulse; 22-28s up distance pulse\n";
}

template <typename Update>
void run_realtime(const double duration, Update&& update) {
  const auto begin = steady_clock::now();
  auto previous = begin;
  int printed_second = -1;
  for (;;) {
    const auto now = steady_clock::now();
    const double elapsed = std::chrono::duration<double>(now - begin).count();
    if (elapsed >= duration) break;
    const double delta = std::chrono::duration<double>(now - previous).count();
    previous = now;
    const vec3 pos = trajectory(elapsed);
    update(elapsed, delta, pos);

    const int second = static_cast<int>(elapsed);
    if (second != printed_second) {
      printed_second = second;
      std::cout << std::fixed << std::setprecision(2) << "t=" << elapsed << "  "
                << std::setw(20) << phase_name(elapsed) << "  pos=("
                << pos.x << ", " << pos.y << ", " << pos.z << ") r="
                << source_distance(pos) << '\n';
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
  }
}

void run_miniaudio(const options& opts, const std::vector<int16_t>& pcm) {
  std::vector<std::string> devices;
  devils_engine::sound::system2::playback_devices(devices);
  const std::string selected = select_device(devices, opts.device);
  devils_engine::sound::system2 engine(selected, 1.0, sample_rate);
  std::cout << "Backend: miniaudio system2; device: " << engine.playback_device_name() << '\n'
            << "Output: " << engine.playback_rate() << " Hz, "
            << engine.playback_channel_count() << " channels\n"
            << "HRTF: no separate switch in current miniaudio engine spatializer\n";
  engine.set_master_volume(opts.gain);
  engine.set_listener_pos(vec3(0.0f, 0.0f, 0.0f));
  engine.set_listener_ori(vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
  engine.set_listener_vel(vec3(0.0f, 0.0f, 0.0f));

  auto blob = std::make_shared<devils_engine::sound::resource_blob>();
  blob->id = "audio_spatial_lab/reference";
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
  task.min_distance = opts.min_distance;
  task.max_distance = opts.max_distance;
  if (!engine.setup_sound(task)) throw std::runtime_error("miniaudio rejected reference task");

  run_realtime(opts.duration, [&](const double, const double delta, const vec3& pos) {
    const auto micros = static_cast<size_t>(std::max(1.0, delta * 1'000'000.0));
    engine.update(micros);
    devils_engine::sound::task_update change{};
    change.id = task.id;
    change.pos = pos;
    engine.update_sound(change);
  });
  engine.remove_sound(task.id);
}

void require_al(const std::string_view operation) {
  const ALenum error = alGetError();
  if (error != AL_NO_ERROR) {
    throw std::runtime_error(std::string(operation) + " failed with OpenAL error " + std::to_string(error));
  }
}

class openal_session {
public:
  openal_session(const options& opts, const std::vector<int16_t>& pcm) try {
    const auto devices = openal_devices();
    selected_ = select_device(devices, opts.device);
    device_ = alcOpenDevice(selected_.empty() ? nullptr : selected_.c_str());
    if (device_ == nullptr) throw std::runtime_error("could not open OpenAL playback device");
    if (selected_.empty()) {
      if (const ALCchar* actual = alcGetString(device_, ALC_DEVICE_SPECIFIER)) selected_ = actual;
    }

    const bool has_hrtf = alcIsExtensionPresent(device_, "ALC_SOFT_HRTF") == ALC_TRUE;
    std::array<ALCint, 3> attrs{};
    const ALCint* attr_ptr = nullptr;
    if (opts.hrtf != hrtf_mode::automatic) {
      if (!has_hrtf && opts.hrtf == hrtf_mode::enabled) {
        throw std::runtime_error("OpenAL device does not expose ALC_SOFT_HRTF");
      }
      if (has_hrtf) {
        attrs = {ALC_HRTF_SOFT, opts.hrtf == hrtf_mode::enabled ? ALC_TRUE : ALC_FALSE, 0};
        attr_ptr = attrs.data();
      }
    }
    context_ = alcCreateContext(device_, attr_ptr);
    if (context_ == nullptr || alcMakeContextCurrent(context_) != ALC_TRUE) {
      throw std::runtime_error("could not create/make current OpenAL context");
    }

    ALCint actual_hrtf = ALC_FALSE;
    if (has_hrtf) alcGetIntegerv(device_, ALC_HRTF_SOFT, 1, &actual_hrtf);
    ALCint output_rate = 0;
    alcGetIntegerv(device_, ALC_FREQUENCY, 1, &output_rate);
    std::cout << "Backend: OpenAL; device: " << (selected_.empty() ? "<unknown>" : selected_) << '\n'
              << "Output: " << output_rate << " Hz; channel layout is backend/device-selected\n"
              << "HRTF extension: " << (has_hrtf ? "yes" : "no")
              << "; active: " << (actual_hrtf == ALC_TRUE ? "yes" : "no");
    if (actual_hrtf == ALC_TRUE) {
      if (const ALCchar* spec = alcGetString(device_, ALC_HRTF_SPECIFIER_SOFT)) {
        std::cout << " (" << spec << ")";
      }
    }
    std::cout << '\n';

    alDistanceModel(AL_LINEAR_DISTANCE_CLAMPED);
    alDopplerFactor(0.0f);
    alListener3f(AL_POSITION, 0.0f, 0.0f, 0.0f);
    alListener3f(AL_VELOCITY, 0.0f, 0.0f, 0.0f);
    const ALfloat orientation[6] = {0.0f, 0.0f, -1.0f, 0.0f, 1.0f, 0.0f};
    alListenerfv(AL_ORIENTATION, orientation);
    alListenerf(AL_GAIN, opts.gain);

    alGenBuffers(1, &buffer_);
    alBufferData(buffer_, AL_FORMAT_MONO16, pcm.data(),
                 static_cast<ALsizei>(pcm.size() * sizeof(pcm[0])), sample_rate);
    alGenSources(1, &source_);
    alSourcei(source_, AL_BUFFER, static_cast<ALint>(buffer_));
    alSourcei(source_, AL_LOOPING, AL_FALSE);
    alSourcei(source_, AL_SOURCE_RELATIVE, AL_FALSE);
    alSourcef(source_, AL_GAIN, 1.0f);
    alSourcef(source_, AL_REFERENCE_DISTANCE, opts.min_distance);
    alSourcef(source_, AL_MAX_DISTANCE, opts.max_distance);
    alSourcef(source_, AL_ROLLOFF_FACTOR, 1.0f);
    const vec3 pos = trajectory(0.0);
    alSource3f(source_, AL_POSITION, pos.x, pos.y, pos.z);
    require_al("OpenAL setup");
  } catch (...) {
    cleanup();
    throw;
  }

  ~openal_session() {
    cleanup();
  }

  void play() {
    alSourcePlay(source_);
    require_al("alSourcePlay");
  }

  void set_position(const vec3& pos) {
    alSource3f(source_, AL_POSITION, pos.x, pos.y, pos.z);
    require_al("alSource3f");
  }

private:
  void cleanup() noexcept {
    if (source_ != 0) {
      alSourceStop(source_);
      alDeleteSources(1, &source_);
      source_ = 0;
    }
    if (buffer_ != 0) {
      alDeleteBuffers(1, &buffer_);
      buffer_ = 0;
    }
    if (context_ != nullptr) {
      alcMakeContextCurrent(nullptr);
      alcDestroyContext(context_);
      context_ = nullptr;
    }
    if (device_ != nullptr) {
      alcCloseDevice(device_);
      device_ = nullptr;
    }
  }

  std::string selected_;
  ALCdevice* device_ = nullptr;
  ALCcontext* context_ = nullptr;
  ALuint buffer_ = 0;
  ALuint source_ = 0;
};

void run_openal(const options& opts, const std::vector<int16_t>& pcm) {
  openal_session session(opts, pcm);
  session.play();
  run_realtime(opts.duration, [&](const double, const double, const vec3& pos) {
    session.set_position(pos);
  });
}

void print_dry_run(const options& opts, const std::vector<int16_t>& pcm) {
  validate_trajectory();
  std::cout << "Dry run: no device opened\n";
  print_run_header(opts, pcm);
  const double end = std::min(opts.duration, scenario_duration);
  for (double t = 0.0; t < end; t += 1.0) {
    const auto pos = trajectory(t);
    std::cout << std::fixed << std::setprecision(2) << "t=" << t << " " << phase_name(t)
              << " pos=(" << pos.x << ", " << pos.y << ", " << pos.z << ") r="
              << source_distance(pos) << '\n';
  }
}

} // namespace

int main(int argc, char** argv) {
  try {
    const options opts = parse_options(argc, argv);
    if (opts.list_devices) {
      print_devices();
      return 0;
    }

    const auto pcm = reference_signal(opts.duration);
    if (opts.dry_run) {
      print_dry_run(opts, pcm);
      return 0;
    }

    print_run_header(opts, pcm);
    if (opts.backend == backend_kind::miniaudio) run_miniaudio(opts, pcm);
    else run_openal(opts, pcm);
    std::cout << "Pass complete. Run the other backend with identical arguments.\n";
    return 0;
  } catch (const std::exception& e) {
    std::cerr << "audio_spatial_lab: " << e.what() << '\n';
    return 1;
  }
}
