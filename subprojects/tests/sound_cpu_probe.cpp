#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iostream>
#include <thread>
#include <sys/resource.h>
#include "devils_engine/sound/system.h"
#include "devils_engine/utils/error.h"

using namespace devils_engine;
namespace {
using clock_type = std::chrono::steady_clock;
double elapsed(clock_type::time_point start) { return std::chrono::duration<double, std::milli>(clock_type::now() - start).count(); }
size_t number(const char* text) {
  size_t value = 0;
  const auto end = text + std::strlen(text);
  const auto result = std::from_chars(text, end, value);
  if (result.ec != std::errc{} || result.ptr != end) utils::error{}("sound_cpu_probe: invalid integer '{}'", text);
  return value;
}
void u16(std::vector<char>& into, uint16_t value) { into.push_back(char(value)); into.push_back(char(value >> 8)); }
void u32(std::vector<char>& into, uint32_t value) { for (size_t i = 0; i < 4; ++i) into.push_back(char(value >> (8 * i))); }
std::shared_ptr<sound::resource_blob> generated(std::string mode, uint32_t rate, uint16_t channels) {
  auto blob = std::make_shared<sound::resource_blob>();
  blob->id = mode;
  blob->channels = channels;
  blob->sample_rate = rate;
  blob->frames_count = 3 * rate;
  blob->type = mode == "pcm" ? sound::data_type::pcm : sound::data_type::wav;
  blob->sample_format = sound::format::f32;
  if (mode == "pcm") {
    blob->data.resize(blob->frames_count * channels * sizeof(float));
    for (size_t i = 0; i < blob->frames_count; ++i) {
      const float sample = 0.01f * std::sin(float(i) * 440.0f * 6.28318530718f / rate);
      for (size_t c = 0; c < channels; ++c) std::memcpy(blob->data.data() + (i * channels + c) * sizeof(float), &sample, sizeof(sample));
    }
  } else {
    auto& data = blob->data;
    auto tag = [&](const char* word) { data.insert(data.end(), word, word + 4); };
    const uint32_t bytes = uint32_t(blob->frames_count * channels * 2);
    tag("RIFF"); u32(data, 36 + bytes); tag("WAVE"); tag("fmt "); u32(data, 16);
    u16(data, 1); u16(data, channels); u32(data, rate); u32(data, rate * channels * 2);
    u16(data, channels * 2); u16(data, 16); tag("data"); u32(data, bytes);
    for (size_t i = 0; i < blob->frames_count; ++i) {
      const auto sample = uint16_t(int16_t(300 * std::sin(double(i) * 440 * 6.28318530718 / rate)));
      for (size_t c = 0; c < channels; ++c) u16(data, sample);
    }
  }
  return blob;
}
std::shared_ptr<sound::resource_blob> mp3() {
  const std::string path = SOUND_PROBE_MP3;
  std::ifstream input(path, std::ios::binary);
  if (!input) utils::error{}("sound_cpu_probe: cannot read '{}'", path);
  auto blob = std::make_shared<sound::resource_blob>();
  blob->id = path;
  blob->type = sound::data_type::mp3;
  blob->data.assign(std::istreambuf_iterator<char>(input), {});
  return blob;
}
double percentile(std::vector<double> values, double p) {
  std::sort(values.begin(), values.end());
  return values[size_t((values.size() - 1) * p)];
}
}
int main(int argc, char** argv) {
  if (argc < 5 || argc > 7) { std::cerr << "sound_cpu_probe VOICES pcm|wav|mp3 INPUT_RATE BUFFER_MILLISECONDS [owner|device] [flat|spatial]\n"; return 2; }
  const bool spatial = argc == 7 && std::string_view(argv[6]) == "spatial";
  if (argc == 7 && !spatial && std::string_view(argv[6]) != "flat") utils::error{}("sound_cpu_probe: expected flat or spatial");
  const size_t voices = number(argv[1]);
  const std::string mode = argv[2];
  uint32_t rate = uint32_t(number(argv[3]));
  const double buffer_seconds = double(number(argv[4])) / 1000;
  if (voices > 320 || rate == 0 || (mode != "pcm" && mode != "wav" && mode != "mp3")) utils::error{}("sound_cpu_probe: invalid workload");
  auto mono = mode == "mp3" ? mp3() : generated(mode, rate, 1);
  auto stereo = mode == "mp3" ? mono : generated(mode, rate, 2);
  if (mode == "mp3") { auto metadata = sound::make_decoder(mono->type, mono->id, mono->data); rate = metadata->sample_rate(); }
  auto started = clock_type::now();
  sound::system_config config;
  config.offline = true;
  config.mixing = argc >= 6 ? sound::parse_mixing_mode(argv[5]) : sound::mixing_mode::device;
  config.stream_buffer_seconds = buffer_seconds;
  sound::system audio(config);
  const double init_ms = elapsed(started);
  sound::directional_coloration_config coloration;
  coloration.enabled = true;
  audio.set_directional_coloration(coloration);
  const size_t stereo_voices = voices / 5;
  for (size_t i = 0; i < voices; ++i) {
    const bool positioned = i >= stereo_voices;
    sound::task task(i, sound::resource2(positioned ? mono : stereo));
    task.type = positioned ? sound::type::sfx : sound::type::ui_effect;
    task.volume = 0.1f;
    task.max_distance = positioned && spatial ? sound::default_sound_max_distance : 0.0f;
    task.min_distance = 1.0f;
    task.pos = {float(i % 7 + 1), 1, float(i % 3)};
    if (!audio.setup_sound(task)) utils::error{}("sound_cpu_probe: task {} refused", i);
  }
  started = clock_type::now();
  audio.update(0);
  const double cold_ms = elapsed(started);
  std::vector<float> pcm(800 * audio.playback_channel_count());
  audio.render(pcm);
  std::vector<double> updates, mixes, totals;
  double checksum = 0;
  std::vector<sound::task_update> movement(voices);
  for (size_t i = 0; i < voices; ++i) { movement[i].id = i; movement[i].pos = {float(i % 7 + 1), 1, float(i % 3)}; }
  for (size_t i = 0; i < 90; ++i) {
    const auto all = clock_type::now();
    started = clock_type::now();
    audio.set_listener_pos({float(i % 11) * .01f, 0, 0});
    for (auto& move : movement) audio.update_sound(move);
    audio.update(16667);
    updates.push_back(elapsed(started));
    started = clock_type::now(); audio.render(pcm); mixes.push_back(elapsed(started));
    totals.push_back(elapsed(all));
    checksum += pcm[i % pcm.size()];
  }
  std::vector<sound::task_status> states;
  audio.snapshot(states);
  size_t underruns = 0;
  for (const auto& state : states) underruns += state.underruns;
  rusage usage{};
  getrusage(RUSAGE_SELF, &usage);
  std::cout << "{\"voices\":" << voices << ",\"mode\":\"" << mode << "\",\"input_rate\":" << rate
            << ",\"buffer_ms\":" << buffer_seconds * 1000 << ",\"hardware_threads\":" << std::thread::hardware_concurrency()
            << ",\"mixing\":\"" << (config.mixing == sound::mixing_mode::owner ? "owner" : "device") << "\""
            << ",\"spatial\":" << (spatial ? "true" : "false")
            << ",\"init_ms\":" << init_ms << ",\"cold_update_ms\":" << cold_ms
            << ",\"update_p50_ms\":" << percentile(updates, .5) << ",\"update_p95_ms\":" << percentile(updates, .95)
            << ",\"update_max_ms\":" << *std::max_element(updates.begin(), updates.end())
            << ",\"mix_p50_ms\":" << percentile(mixes, .5) << ",\"mix_p95_ms\":" << percentile(mixes, .95)
            << ",\"total_p95_ms\":" << percentile(totals, .95) << ",\"total_max_ms\":" << *std::max_element(totals.begin(), totals.end())
            << ",\"active_tasks\":" << states.size() << ",\"underruns\":" << underruns
            << ",\"max_rss_kib\":" << usage.ru_maxrss << ",\"checksum\":" << checksum << "}\n";
}
