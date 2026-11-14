#include "devils_engine/utils/log.h"
#include <algorithm>
#include <atomic>
#include <cassert>
#include <cmath>
#include <cstring>

#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#define MINIAUDIO_IMPLEMENTATION
#include <miniaudio.h>

#include <devils_engine/catalogue/logging.h>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/time-utils.hpp" // utils::global_time_resolution — µs-единица дедлайнов задач
#include "flac_decoder.h"
#include "mp3_decoder.h"
#include "ogg_decoder.h"
#include "opus_decoder.h"
#include "pcm_decoder.h"
#include "system.h"
#include "wav_decoder.h"

//#define SOUND_LOADING_COEFFICIENT 1.0f

namespace devils_engine {
namespace sound {

struct system::miniaudio_data_converter : public ma_data_converter {
  bool initialized = false;
  ~miniaudio_data_converter() { if (initialized) ma_data_converter_uninit(this, nullptr); }
};

static_assert(static_cast<uint32_t>(format::u8) == ma_format_u8);
static_assert(static_cast<uint32_t>(format::s16) == ma_format_s16);
static_assert(static_cast<uint32_t>(format::s24) == ma_format_s24);
static_assert(static_cast<uint32_t>(format::s32) == ma_format_s32);
static_assert(static_cast<uint32_t>(format::f32) == ma_format_f32);

vec3::vec3() noexcept : vec3(0.0f, 0.0f, 0.0f) {}
vec3::vec3(const float x, const float y, const float z) noexcept : x(x), y(y), z(z) {}

float distance2(const vec3& a, const vec3& b) noexcept {
  const auto diff = a - b;
  return dot2(diff, diff);
  //return (a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z);
}

vec3 operator-(const vec3& a, const vec3& b) noexcept {
  return vec3(a.x - b.x, a.y - b.y, a.z - b.z);
}

float dot2(const vec3& a, const vec3& b) noexcept {
  return a.x * b.x + a.y * b.y + a.z * b.z;
}

vec3 normalize(const vec3& a) noexcept {
  const float length2 = dot2(a, a);
  if (length2 <= 1.0e-12f) {
    return vec3();
  }
  const float k = std::sqrt(length2);
  return vec3(a.x / k, a.y / k, a.z / k);
}

directional_coloration_config sanitize_directional_coloration_config(
  directional_coloration_config config) noexcept {
  config.strength = std::clamp(config.strength, 0.0f, 2.0f);
  config.behind_high_shelf_db = std::clamp(config.behind_high_shelf_db, -3.0f, 0.0f);
  config.above_high_shelf_db = std::clamp(config.above_high_shelf_db, -1.0f, 1.0f);
  config.below_high_shelf_db = std::clamp(config.below_high_shelf_db, -1.0f, 1.0f);
  config.high_shelf_frequency_hz = std::clamp(config.high_shelf_frequency_hz, 500.0f, 12000.0f);
  config.smoothing_ms = std::clamp(config.smoothing_ms, 10.0f, 500.0f);
  return config;
}

static float directional_weight(const float value) noexcept {
  const float x = std::clamp(value, 0.0f, 1.0f);
  return x * x * (3.0f - 2.0f * x);
}

directional_coloration_response compute_directional_coloration(
  const directional_coloration_config& unsanitized_config,
  const vec3& listener_pos,
  const vec3& listener_forward,
  const vec3& listener_up,
  const vec3& source_pos) noexcept {
  directional_coloration_response response;
  const auto config = sanitize_directional_coloration_config(unsanitized_config);
  if (!config.enabled) {
    return response;
  }

  const vec3 relative = normalize(source_pos - listener_pos);
  const vec3 forward = normalize(listener_forward);
  const vec3 up = normalize(listener_up);
  if (dot2(relative, relative) == 0.0f || dot2(forward, forward) == 0.0f || dot2(up, up) == 0.0f) {
    return response;
  }

  const float front_dot = std::clamp(dot2(relative, forward), -1.0f, 1.0f);
  const float up_dot = std::clamp(dot2(relative, up), -1.0f, 1.0f);
  response.behind = directional_weight(-front_dot);
  response.above = directional_weight(up_dot);
  response.below = directional_weight(-up_dot);
  response.high_shelf_db = std::clamp(
    config.strength * (
      response.behind * config.behind_high_shelf_db +
      response.above * config.above_high_shelf_db +
      response.below * config.below_high_shelf_db),
    -3.0f, 1.0f);
  return response;
}

double compute_base_priority(const enum type type) noexcept {
  return double(static_cast<size_t>(type)) / double(static_cast<size_t>(type::count));
}

std::unique_ptr<decoder> make_decoder(const data_type type, const std::string_view& name, const std::span<const char>& data) {
  switch (type) {
    case data_type::mp3: return std::make_unique<mp3_decoder>(name, data.data(), data.size());
    case data_type::wav: return std::make_unique<wav_decoder>(name, data.data(), data.size());
    case data_type::flac: return std::make_unique<flac_decoder>(name, data.data(), data.size());
    case data_type::ogg: return std::make_unique<ogg_decoder>(name, data.data(), data.size());
    case data_type::opus: return std::make_unique<opus_decoder>(name, data.data(), data.size());
    //case data_type::pcm:  return std::make_unique<pcm_decoder>(name, data.data(), data.size());
    case data_type::pcm: return std::unique_ptr<decoder>();
    case data_type::undefined: return std::unique_ptr<decoder>();
  }

  return std::unique_ptr<decoder>();
}

task::task() noexcept : task(SIZE_MAX, {}) {}
task::task(const size_t id, const resource2& res) noexcept : id(id),
                                                             res(res),
                                                             command(command::play),
                                                             type(type::sfx),
                                                             pitch(1.0f),
                                                             volume(1.0f),
                                                             start(0.0),
                                                             after(SIZE_MAX),
                                                             min_distance(0.0f),
                                                             max_distance(0.0f) {}

struct devils_engine_sound_data_source {
  ma_data_source_base base;

  std::vector<float> buffer;
  std::atomic<size_t> write_pos;
  std::atomic<size_t> read_pos;
  std::atomic<size_t> cursor_pos;
  std::atomic<size_t> frames_read_total;
  std::atomic<size_t> frames_written_total;
  std::atomic<size_t> underrun_count;

  size_t current_cursor_pos;

  // never change once initialized
  uint32_t sample_rate;
  uint32_t channels;
  enum format format;

  devils_engine_sound_data_source() noexcept : base{},
                                               write_pos(0),
                                               read_pos(0),
                                               cursor_pos(0),
                                               frames_read_total(0),
                                               frames_written_total(0),
                                               underrun_count(0),
                                               current_cursor_pos(0),
                                               sample_rate(0),
                                               channels(0),
                                               format(format::unknown) {}

  void reset_stream() noexcept {
    read_pos.store(0, std::memory_order_release);
    write_pos.store(0, std::memory_order_release);
    cursor_pos.store(0, std::memory_order_release);
    frames_read_total.store(0, std::memory_order_release);
    frames_written_total.store(0, std::memory_order_release);
    underrun_count.store(0, std::memory_order_release);
    current_cursor_pos = 0;
  }

  size_t buffer_size() const noexcept {
    //return bytes_to_pcm_frames(this->buffer.size() * sizeof(this->buffer[0]), this->channels, this->format);
    return buffer.size() * sizeof(buffer[0]);
  }

  size_t available_frames_to_write() const noexcept {
    const size_t write_pos = this->write_pos.load(std::memory_order_relaxed);
    const size_t read_pos = this->read_pos.load(std::memory_order_acquire);
    const size_t free_bytes = buffer_size() - (write_pos - read_pos);
    return bytes_to_pcm_frames(free_bytes, channels, format);
  }

  devils_engine_sound_data_source(const devils_engine_sound_data_source& copy) noexcept = delete;
  devils_engine_sound_data_source(devils_engine_sound_data_source&& move) noexcept = delete;
  devils_engine_sound_data_source& operator=(const devils_engine_sound_data_source& copy) noexcept = delete;
  devils_engine_sound_data_source& operator=(devils_engine_sound_data_source&& move) noexcept = delete;
};

static_assert(offsetof(devils_engine_sound_data_source, base) == 0);

// надо все переделать на фреймы ...........

static ma_result my_data_source_read(ma_data_source* pDataSource, void* pFramesOut, ma_uint64 frameCount, ma_uint64* pFramesRead) {
  auto ptr = reinterpret_cast<devils_engine_sound_data_source*>(pDataSource);
  auto out_buffer = reinterpret_cast<uint8_t*>(pFramesOut);
  auto in_buffer = reinterpret_cast<const uint8_t*>(ptr->buffer.data());
  //const size_t buffer_size = bytes_to_pcm_frames(ptr->buffer.size() * sizeof(ptr->buffer[0]), ptr->channels, ptr->format);
  const size_t buffer_size = ptr->buffer_size();

  const size_t bytesCount = pcm_samples_to_bytes(frameCount, ptr->channels, ptr->format);
  //utils::info("Data source read {} frames ({} samples, {} bytes)", frameCount, samplesCount, bytesCount);

  // что тут мы делаем:
  // получаем позицию кольцевого буфера (сначала read потом write)
  //const size_t samples_needed = frameCount * ptr->channels;
  const size_t read_pos = ptr->read_pos.load(std::memory_order_relaxed);
  const size_t write_pos = ptr->write_pos.load(std::memory_order_acquire);

  // позиция записи - позиция чтения = сколько всего фреймов доступно (наверное должно быть в байтах)
  // нужно прочитать только samples_needed
  const size_t available = write_pos - read_pos;
  const size_t to_read = std::min(bytesCount, available);
  const size_t buffer_read_pos = read_pos % buffer_size;

  // первая часть от read_pos до buffer.end()
  const size_t first_part = std::min(to_read, buffer_size - buffer_read_pos);

  //utils::info("Can read {}, first part {}, read pos {}, write pos {}", to_read, first_part, read_pos, write_pos);
  memcpy(out_buffer, in_buffer + buffer_read_pos, first_part);

  // вторая часть от buffer.begin() до (to_read - first_part)
  const size_t second_part = to_read - first_part;
  if (second_part > 0) {
    memcpy(out_buffer + first_part, in_buffer, second_part);
  }

  // если не хватает сэмплов заполним тишиной
  if (to_read < bytesCount) {
    memset(out_buffer + to_read, 0, bytesCount - to_read);
    ptr->underrun_count.fetch_add(1, std::memory_order_relaxed);
  }

  // двигаем указатель чтения
  ptr->read_pos.store(read_pos + to_read, std::memory_order_release);

  const size_t final_frames_read = bytes_to_pcm_frames(to_read, ptr->channels, ptr->format);
  ptr->current_cursor_pos += final_frames_read;
  ptr->cursor_pos.store(ptr->current_cursor_pos, std::memory_order_release);
  ptr->frames_read_total.fetch_add(final_frames_read, std::memory_order_release);

  // может быть стриминг... вообще для стриминга генерировать бы пакеты вне my_data_source_read
  // мы бы хотели добавить шумы
  if (to_read == 0) {
    if (pFramesRead) {
      *pFramesRead = 0;
    }
    return MA_AT_END;
  }

  if (pFramesRead) {
    *pFramesRead = final_frames_read;
  }

  return MA_SUCCESS;
}

static size_t write_decoded_pcm_frames(devils_engine_sound_data_source* src, const void* data, const size_t frame_count) {
  auto in = reinterpret_cast<const uint8_t*>(data);
  auto out = reinterpret_cast<uint8_t*>(src->buffer.data());
  const size_t buffer_size = src->buffer.size() * sizeof(src->buffer[0]);

  const size_t bytesCount = pcm_samples_to_bytes(frame_count, src->channels, src->format);

  // получаем позицию кольцевого буфера (сначала write потом read)
  const size_t write_pos = src->write_pos.load(std::memory_order_relaxed);
  const size_t read_pos = src->read_pos.load(std::memory_order_acquire);

  const size_t free = buffer_size - (write_pos - read_pos);
  const size_t to_write = std::min(bytesCount, free);
  const size_t buffer_write_pos = write_pos % buffer_size;

  // memcpy (with wrap)
  const size_t first_part = std::min(to_write, buffer_size - buffer_write_pos);
  memcpy(out + buffer_write_pos, in, first_part);

  // вторая часть от buffer.begin() до (to_read - first_part)
  const size_t second_part = to_write - first_part;
  if (second_part > 0) {
    memcpy(out, in + first_part, second_part);
  }

  src->write_pos.store(write_pos + to_write, std::memory_order_release);

  const size_t written_frames = bytes_to_pcm_frames(to_write, src->channels, src->format);
  src->frames_written_total.fetch_add(written_frames, std::memory_order_release);
  return written_frames;
}

// поиск в фреймах (сэмплы * каналы)
static ma_result my_data_source_seek(ma_data_source* pDataSource, ma_uint64 frameIndex) {
  auto* ptr = reinterpret_cast<devils_engine_sound_data_source*>(pDataSource);
  const size_t frame_bytes = pcm_frame_to_bytes(ptr->channels, ptr->format);
  const size_t byte_pos = frameIndex * frame_bytes;
  ptr->read_pos.store(byte_pos, std::memory_order_release);
  ptr->write_pos.store(byte_pos, std::memory_order_release);
  ptr->cursor_pos.store(frameIndex, std::memory_order_release);
  ptr->frames_read_total.store(frameIndex, std::memory_order_release);
  ptr->frames_written_total.store(frameIndex, std::memory_order_release);
  ptr->current_cursor_pos = frameIndex;

  //utils::info("Data source seek");

  return MA_SUCCESS;
}

static ma_result my_data_source_get_data_format(ma_data_source* pDataSource, ma_format* pFormat, ma_uint32* pChannels, ma_uint32* pSampleRate, ma_channel* pChannelMap, size_t channelMapCap) {
  auto* ptr = reinterpret_cast<devils_engine_sound_data_source*>(pDataSource);
  if (pFormat) {
    *pFormat = static_cast<ma_format>(ptr->format);
  }
  if (pChannels) {
    *pChannels = ptr->channels;
  }
  if (pSampleRate) {
    *pSampleRate = ptr->sample_rate;
  }

  if (pChannelMap != nullptr && channelMapCap >= ptr->channels) {
    if (ptr->channels == 1) {
      pChannelMap[0] = MA_CHANNEL_MONO;
    } else if (ptr->channels == 2) {
      pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
      pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
    } else if (ptr->channels == 6) {
      pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
      pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
      pChannelMap[2] = MA_CHANNEL_FRONT_CENTER;
      pChannelMap[3] = MA_CHANNEL_LFE;
      pChannelMap[4] = MA_CHANNEL_BACK_LEFT;
      pChannelMap[5] = MA_CHANNEL_BACK_RIGHT;
    } else if (ptr->channels == 8) {
      pChannelMap[0] = MA_CHANNEL_FRONT_LEFT;
      pChannelMap[1] = MA_CHANNEL_FRONT_RIGHT;
      pChannelMap[2] = MA_CHANNEL_FRONT_CENTER;
      pChannelMap[3] = MA_CHANNEL_LFE;
      pChannelMap[4] = MA_CHANNEL_BACK_LEFT;
      pChannelMap[5] = MA_CHANNEL_BACK_RIGHT;
      pChannelMap[6] = MA_CHANNEL_SIDE_LEFT;
      pChannelMap[7] = MA_CHANNEL_SIDE_RIGHT;
    } else {
      for (ma_uint32 i = 0; i < ptr->channels; ++i) {
        pChannelMap[i] = MA_CHANNEL_NONE;
      }
    }
  }

  //utils::info("Data source get_data_format: {} {} {}", static_cast<size_t>(ptr->format), ptr->channels, ptr->sample_rate);

  return MA_SUCCESS;
}

// зацикленный звук надо бы вычитать из курсора
// то есть для каждого прочитанного файла увеличивать дополнительный счетчик и тут его вычитать
static ma_result my_data_source_get_cursor(ma_data_source* pDataSource, ma_uint64* pCursor) {
  auto* ptr = reinterpret_cast<devils_engine_sound_data_source*>(pDataSource);

  *pCursor = ptr->cursor_pos.load(std::memory_order_acquire);

  DE_LOG(catalogue::log_domain::sound, trace, "Data source get_cursor");

  return MA_SUCCESS;
}

// возвращаю именно фреймы (сэмплы * каналы)
static ma_result my_data_source_get_length(ma_data_source* pDataSource, ma_uint64* pLength) {
  auto* ptr = reinterpret_cast<devils_engine_sound_data_source*>(pDataSource);
  // тут мы должны понять какой именно источник данных у нас скрыт в ресурсе
  // (например если VOIP то мы будем заполнять потихоньку буфер данных в ресурсе)
  // пока что такого нет

  *pLength = ptr->frames_written_total.load(std::memory_order_acquire);

  DE_LOG(catalogue::log_domain::sound, trace, "Data source get_length");

  return MA_SUCCESS;
}

static ma_data_source_vtable g_data_source_vtable = {
  my_data_source_read,
  my_data_source_seek,
  my_data_source_get_data_format,
  my_data_source_get_cursor,
  my_data_source_get_length,
  nullptr,
  0};

static ma_result standart_source_init(devils_engine_sound_data_source* pMyDataSource, const uint32_t sample_rate, const uint32_t channels, const enum format format, const double buffer_seconds) {
  auto baseConfig = ma_data_source_config_init();
  baseConfig.vtable = &g_data_source_vtable;

  const auto result = ma_data_source_init(&baseConfig, &pMyDataSource->base);
  if (result != MA_SUCCESS) {
    return result;
  }

  pMyDataSource->format = format;
  pMyDataSource->channels = channels;
  pMyDataSource->sample_rate = sample_rate;

  // Кольцо НЕ выделяем здесь — ленивое выделение при первом реальном использовании голоса
  // (ensure_stream_buffer). Иначе 320 предсозданных голосов держат сотни МБ вхолостую.
  // (buffer_seconds сохранять не нужно: он один на систему = stream_buffer_seconds.)
  static_cast<void>(buffer_seconds);
  pMyDataSource->read_pos = 0;
  pMyDataSource->write_pos = 0;

  return MA_SUCCESS;
}





// Ленивое выделение PCM-кольца голоса. ВАЖНО: buffer — vector<float>, а seconds_to_bytes даёт
// БАЙТЫ, поэтому число элементов = байты / sizeof(float) (раньше resize звался числом БАЙТ →
// кольцо было в 4× больше задуманного). Резайзим только если текущее меньше нужного.
static void ensure_stream_buffer(devils_engine_sound_data_source* ds, const double buffer_seconds) {
  const size_t bytes = seconds_to_bytes(buffer_seconds, ds->sample_rate, ds->channels, ds->format);
  const size_t elements = (bytes + sizeof(float) - 1) / sizeof(float);
  if (ds->buffer.size() < elements) {
    ds->buffer.resize(elements, 0.0f);
  }
}

//    static ma_result sound_instance_init(system::sound_instance* ptr, const uint32_t sample_rate, const uint32_t channels, const enum format format) {
//
//    }

static void data_source_uninit(devils_engine_sound_data_source* pMyDataSource) {
  // ... do the uninitialization of your custom data source here ...

  ma_data_source_uninit(&pMyDataSource->base);
}

// A tiny post-spatialization shelf. The sound thread only publishes atomic targets; coefficient
// smoothing and filter state belong exclusively to miniaudio's audio callback. This avoids
// reinitializing a stock filter concurrently with graph processing.
struct directional_coloration_node {
  ma_node_base base{}; // must remain first: miniaudio treats the object as ma_node
  ma_uint32 channels = 0;
  ma_uint32 sample_rate = 0;
  std::atomic<float> target_high_gain{1.0f};
  std::atomic<float> smoothing_alpha{1.0f};
  std::atomic<float> lowpass_alpha{1.0f};
  float current_high_gain = 1.0f;
  std::array<float, MA_MAX_CHANNELS> lowpass_state{};
};

static void directional_coloration_process(
  ma_node* node,
  const float** frames_in,
  ma_uint32* frame_count_in,
  float** frames_out,
  ma_uint32* frame_count_out) {
  auto* coloration = reinterpret_cast<directional_coloration_node*>(node);
  const ma_uint32 frames = std::min(*frame_count_in, *frame_count_out);
  const float target_gain = coloration->target_high_gain.load(std::memory_order_relaxed);
  const float smoothing = coloration->smoothing_alpha.load(std::memory_order_relaxed);
  const float lowpass = coloration->lowpass_alpha.load(std::memory_order_relaxed);

  for (ma_uint32 frame = 0; frame < frames; ++frame) {
    coloration->current_high_gain += (target_gain - coloration->current_high_gain) * smoothing;
    const size_t offset = size_t(frame) * coloration->channels;
    for (ma_uint32 channel = 0; channel < coloration->channels; ++channel) {
      const float input = frames_in[0][offset + channel];
      float& low = coloration->lowpass_state[channel];
      low += (input - low) * lowpass;
      frames_out[0][offset + channel] = low + (input - low) * coloration->current_high_gain;
    }
  }

  *frame_count_in = frames;
  *frame_count_out = frames;
}

static const ma_node_vtable directional_coloration_vtable{
  &directional_coloration_process,
  nullptr,
  1,
  1,
  0};

static ma_result directional_coloration_node_init(
  ma_engine* engine,
  directional_coloration_node* node) {
  node->channels = ma_engine_get_channels(engine);
  node->sample_rate = ma_engine_get_sample_rate(engine);
  if (node->channels == 0 || node->channels > MA_MAX_CHANNELS || node->sample_rate == 0) {
    return MA_INVALID_ARGS;
  }

  auto config = ma_node_config_init();
  config.vtable = &directional_coloration_vtable;
  config.pInputChannels = &node->channels;
  config.pOutputChannels = &node->channels;
  return ma_node_init(ma_engine_get_node_graph(engine), &config, nullptr, &node->base);
}

static void directional_coloration_node_set(
  directional_coloration_node* node,
  const directional_coloration_config& config,
  const float target_db) noexcept {
  constexpr float pi = 3.14159265358979323846f;
  const float sample_rate = static_cast<float>(node->sample_rate);
  const float smoothing_frames = std::max(config.smoothing_ms * 0.001f * sample_rate, 1.0f);
  const float smoothing_alpha = 1.0f - std::exp(-1.0f / smoothing_frames);
  const float lowpass_alpha = 1.0f - std::exp(
    -2.0f * pi * config.high_shelf_frequency_hz / sample_rate);
  node->target_high_gain.store(std::pow(10.0f, target_db / 20.0f), std::memory_order_relaxed);
  node->smoothing_alpha.store(std::clamp(smoothing_alpha, 0.0f, 1.0f), std::memory_order_relaxed);
  node->lowpass_alpha.store(std::clamp(lowpass_alpha, 0.0f, 1.0f), std::memory_order_relaxed);
}

struct system::sound_instance {
  ma_sound sound{};
  enum class pool_kind { dynamic, mono, stereo };
  pool_kind pool = pool_kind::dynamic;
  bool sound_initialized = false;
  bool data_source_initialized = false;
  devils_engine_sound_data_source data_source;
  directional_coloration_node coloration;
  bool coloration_initialized = false;

  sound_instance() noexcept = default;
  sound_instance(const sound_instance& copy) noexcept = delete;
  sound_instance(sound_instance&& move) noexcept = delete;
  sound_instance& operator=(const sound_instance& copy) noexcept = delete;
  sound_instance& operator=(sound_instance&& move) noexcept = delete;
};

static ma_result attach_directional_coloration(
  ma_engine* engine,
  system::sound_instance* instance) {
  ma_result result = directional_coloration_node_init(engine, &instance->coloration);
  if (result != MA_SUCCESS) {
    return result;
  }
  instance->coloration_initialized = true;

  result = ma_node_attach_output_bus(
    &instance->coloration.base, 0, ma_engine_get_endpoint(engine), 0);
  if (result != MA_SUCCESS) {
    return result;
  }
  return ma_node_attach_output_bus(
    reinterpret_cast<ma_node*>(&instance->sound), 0,
    reinterpret_cast<ma_node*>(&instance->coloration.base), 0);
}

static void uninit_sound_instance(system::sound_instance* instance) noexcept {
  if (instance->sound_initialized) {
    ma_sound_stop(&instance->sound);
    ma_sound_uninit(&instance->sound);
    instance->sound_initialized = false;
  }
  if (instance->coloration_initialized) {
    ma_node_uninit(&instance->coloration.base, nullptr);
    instance->coloration_initialized = false;
  }
  if (instance->data_source_initialized) {
    data_source_uninit(&instance->data_source);
    instance->data_source_initialized = false;
  }
}

mixing_mode parse_mixing_mode(std::string_view name) {
  if (name == "owner") return mixing_mode::owner;
  if (name == "device") return mixing_mode::device;
  utils::error{}("sound: unknown mixing mode '{}'; expected owner or device", name);
  return mixing_mode::owner;
}

struct system::playback_stream {
  ma_pcm_rb ring{};
  bool initialized = false;
  ~playback_stream() { if (initialized) ma_pcm_rb_uninit(&ring); }
};

void system::playback_callback(ma_device* device, void* output, const void*, uint32_t frames) {
  static_cast<system*>(device->pUserData)->read_output(output, frames);
}

void system::read_output(void* output, uint32_t frames) {
  if (mixing_ == mixing_mode::device) {
    const auto result = ma_engine_read_pcm_frames(m_engine.get(), output, frames, nullptr);
    if (result != MA_SUCCESS) utils::error{}("sound: device mix failed: {}", int(result));
    return;
  }
  auto* target = static_cast<float*>(output);
  uint32_t copied = 0;
  while (copied < frames) {
    uint32_t count = frames - copied;
    void* source = nullptr;
    const auto result = ma_pcm_rb_acquire_read(&output_->ring, &count, &source);
    if (result != MA_SUCCESS) utils::error{}("sound: output ring read failed: {}", int(result));
    if (count == 0) break;
    std::memcpy(target + size_t(copied) * playback_channels, source, size_t(count) * playback_channels * sizeof(float));
    if (ma_pcm_rb_commit_read(&output_->ring, count) != MA_SUCCESS) utils::error{}("sound: output ring read commit failed");
    copied += count;
  }
  // Callback never waits for the owner: a late update produces silence, not graph work here.
  std::fill(target + size_t(copied) * playback_channels, target + size_t(frames) * playback_channels, 0.0f);
}

uint32_t system::queued_output_frames() const noexcept {
  return output_ ? ma_pcm_rb_available_read(&output_->ring) : 0;
}

void system::mix_output() {
  // Single producer on the assigned sound owner, single consumer in the device callback.
  // Fix a finite fill budget even if the callback keeps consuming while we generate.
  uint32_t remaining = ma_pcm_rb_available_write(&output_->ring);
  while (remaining != 0) {
    uint32_t count = remaining;
    void* target = nullptr;
    const auto result = ma_pcm_rb_acquire_write(&output_->ring, &count, &target);
    if (result != MA_SUCCESS) utils::error{}("sound: output ring write failed: {}", int(result));
    if (count == 0) break;
    const auto mix = ma_engine_read_pcm_frames(m_engine.get(), target, count, nullptr);
    if (mix != MA_SUCCESS) utils::error{}("sound: owner mix failed: {}", int(mix));
    if (ma_pcm_rb_commit_write(&output_->ring, count) != MA_SUCCESS) utils::error{}("sound: output ring write commit failed");
    remaining -= count;
  }
}

static bool is_positioned_type(const enum type sound_type) noexcept {
  return sound_type == type::sfx || sound_type == type::talk_pos;
}

static bool is_too_far(const ma_engine* engine, const struct task& task) noexcept {
  if (!is_positioned_type(task.type)) {
    return false;
  }
  const auto pos = ma_engine_listener_get_position(engine, 0);
  const auto dist2 = distance2(task.pos, vec3(pos.x, pos.y, pos.z));
  return dist2 >= default_sound_max_distance * default_sound_max_distance;
}

bool system::playback_devices(std::vector<std::string>& out) {
  out.clear();

  ma_context context;
  auto result = ma_context_init(nullptr, 0, nullptr, &context);
  if (result != MA_SUCCESS) {
    return false;
  }

  ma_device_info* playback_infos = nullptr;
  ma_uint32 playback_count = 0;
  result = ma_context_get_devices(&context, &playback_infos, &playback_count, nullptr, nullptr);
  if (result == MA_SUCCESS) {
    out.reserve(playback_count);
    for (ma_uint32 i = 0; i < playback_count; ++i) {
      out.emplace_back(playback_infos[i].name);
    }
  }

  ma_context_uninit(&context);
  return result == MA_SUCCESS;
}

std::string_view system::playback_device_name() const noexcept {
  return m_device == nullptr ? std::string_view{} : std::string_view(m_device->playback.name);
}

uint32_t system::playback_rate() const noexcept {
  return playback_sample_rate;
}

uint32_t system::playback_channel_count() const noexcept {
  return playback_channels;
}

system::system(const std::string_view device_name, const double stream_buffer_seconds, const size_t decode_frames_per_update)
  : system(system_config{std::string(device_name), stream_buffer_seconds, decode_frames_per_update}) {}

system::system(const system_config& config) : offline_(config.offline), cur_time(0), playback_channels(0), playback_sample_rate(0),
  stream_buffer_seconds(std::max(config.stream_buffer_seconds, 0.05)), decode_frames_per_update(config.decode_frames_per_update) {
  mixing_ = config.mixing;
  source_volume.fill(1.0f);
  try {
    initialize(config);
  } catch (...) {
    shutdown();
    throw;
  }
}

void system::initialize(const system_config& config) {
  if (config.sample_rate == 0 || config.channels == 0 || config.channels > MA_MAX_CHANNELS) utils::error{}("sound: invalid offline format {} Hz / {} channels", config.sample_rate, config.channels);
  if (config.mixing != mixing_mode::owner && config.mixing != mixing_mode::device) utils::error{}("sound: invalid mixing mode");
  if (config.mixing == mixing_mode::owner && (!std::isfinite(config.output_buffer_seconds) || config.output_buffer_seconds < .001 || config.output_buffer_seconds > 1.0)) utils::error{}("sound: output buffer {} seconds is outside [0.001, 1]", config.output_buffer_seconds);
  ma_result result = MA_SUCCESS;
  const std::string_view device_name = config.device_name;
  // Allocate callback data before device creation, but do not start callbacks until
  // engine and voices have been fully initialized.
  m_engine = std::make_unique<ma_engine>();
  // The system owns exactly one selected device. Offline rendering opens none.
  if (!offline_) {
    m_context.reset(new ma_context);
    result = ma_context_init(nullptr, 0, nullptr, m_context.get());
    if (result != MA_SUCCESS) {
      utils::error{}("Could not initialize sound context: {}", int(result));
    }
    context_initialized_ = true;

    const ma_device_id* device_id = nullptr;
    std::string_view default_device_name;
    if (!device_name.empty()) {
      ma_device_info* playback_infos = nullptr;
      ma_uint32 playback_count = 0;
      result = ma_context_get_devices(m_context.get(), &playback_infos, &playback_count, nullptr, nullptr);
      if (result != MA_SUCCESS) {
        utils::error{}("Could not enumerate sound devices");
      }

      for (ma_uint32 i = 0; i < playback_count; ++i) {
        if (playback_infos[i].isDefault) {
          default_device_name = playback_infos[i].name;
        }

        if (std::string_view(playback_infos[i].name) != device_name) {
          continue;
        }
        device_id = &playback_infos[i].id;
        break;
      }

      if (device_id == nullptr) {
        const std::string_view fallback_name = default_device_name.empty() ? std::string_view("<unknown>") : default_device_name;
        utils::warn("Could not find requested sound device '{}'; creating default playback device '{}'", device_name, fallback_name);
      }
    }

    auto cfg = ma_device_config_init(ma_device_type_playback);
    cfg.playback.pDeviceID = device_id;
    cfg.playback.format = ma_format_f32;
    cfg.playback.channels = 0;
    cfg.sampleRate = 0;
    cfg.dataCallback = &system::playback_callback;
    cfg.pUserData = this;

    m_device.reset(new ma_device);
    result = ma_device_init(m_context.get(), &cfg, m_device.get());
    if (result != MA_SUCCESS) {
      utils::error{}("Could not initialize device: {}", int(result));
    }
    device_initialized_ = true;

    utils::info("Using sound device '{}'", std::string_view(m_device->playback.name));

    playback_channels = m_device->playback.channels;
    playback_sample_rate = m_device->sampleRate;
  }

  if (offline_) {
    playback_channels = config.channels;
    playback_sample_rate = config.sample_rate;
  }
  // This engine never uses native file decoding/jobs: resource generations and
  // decoders are owned by sound::system. No unused manager thread is created.
  m_resource_manager = std::make_unique<ma_resource_manager>();
  auto manager_config = ma_resource_manager_config_init();
  manager_config.jobThreadCount = 0;
  manager_config.flags = MA_RESOURCE_MANAGER_FLAG_NO_THREADING;
  result = ma_resource_manager_init(&manager_config, m_resource_manager.get());
  if (result != MA_SUCCESS) utils::error{}("sound: could not initialize synchronous resource manager: {}", int(result));
  resource_manager_initialized_ = true;
  auto engine_config = ma_engine_config_init();
  engine_config.pDevice = m_device.get();
  engine_config.pContext = m_context.get();
  engine_config.pResourceManager = m_resource_manager.get();
  engine_config.noDevice = MA_TRUE;
  engine_config.noAutoStart = MA_TRUE;
  engine_config.listenerCount = 1;
  engine_config.channels = playback_channels;
  engine_config.sampleRate = playback_sample_rate;
  result = ma_engine_init(&engine_config, m_engine.get());
  if (result != MA_SUCCESS) utils::error{}("sound: could not initialize engine at {} Hz / {} channels: {}", playback_sample_rate, playback_channels, int(result));
  engine_initialized_ = true;
  if (ma_engine_get_sample_rate(m_engine.get()) != playback_sample_rate || ma_engine_get_channels(m_engine.get()) != playback_channels) utils::error{}("sound: engine/device format mismatch");

  {
    for (size_t i = 0; i < default_mono_sounds_count; ++i) {
      m_instances_mono.emplace_back(new sound_instance);
      m_instances_mono_stack.push_back(m_instances_mono.back().get());

      auto s = &m_instances_mono.back()->sound;
      auto ds = &m_instances_mono.back()->data_source;

      m_instances_mono.back()->pool = sound_instance::pool_kind::mono;
      result = standart_source_init(ds, playback_sample_rate, 1, format::f32, this->stream_buffer_seconds);
      if (result != MA_SUCCESS) {
        utils::error{}("Could not initialize data source");
      }

      m_instances_mono.back()->data_source_initialized = true;
      // нужно заранее продумать звуковые группы
      result = ma_sound_init_from_data_source(m_engine.get(), ds, 0, nullptr, s);
      if (result != MA_SUCCESS) {
        utils::error{}("Could not initialize sound");
      }
      m_instances_mono.back()->sound_initialized = true;
      result = attach_directional_coloration(m_engine.get(), m_instances_mono.back().get());
      if (result != MA_SUCCESS) {
        utils::error{}("Could not attach directional coloration node");
      }

      ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
      ma_sound_set_rolloff(s, 1.0f);
      ma_sound_set_min_gain(s, 0.0f);
      ma_sound_set_max_gain(s, 1.0f);
      ma_sound_set_volume(s, 1.0f);
      ma_sound_set_min_distance(s, 0.0f);
      ma_sound_set_max_distance(s, default_sound_max_distance);
    }
  }

  {
    for (size_t i = 0; i < default_stereo_sounds_count; ++i) {
      m_instances_stereo.emplace_back(new sound_instance);
      m_instances_stereo_stack.push_back(m_instances_stereo.back().get());

      auto s = &m_instances_stereo.back()->sound;
      auto ds = &m_instances_stereo.back()->data_source;

      m_instances_stereo.back()->pool = sound_instance::pool_kind::stereo;
      result = standart_source_init(ds, playback_sample_rate, 2, format::f32, this->stream_buffer_seconds);
      if (result != MA_SUCCESS) {
        utils::error{}("Could not initialize data source");
      }

      m_instances_stereo.back()->data_source_initialized = true;
      // нужно заранее продумать звуковые группы
      result = ma_sound_init_from_data_source(m_engine.get(), ds, MA_SOUND_FLAG_NO_SPATIALIZATION, nullptr, s);
      if (result != MA_SUCCESS) {
        utils::error{}("Could not initialize sound");
      }

      m_instances_stereo.back()->sound_initialized = true;

      ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
      ma_sound_set_rolloff(s, 1.0f);
      ma_sound_set_min_gain(s, 0.0f);
      ma_sound_set_max_gain(s, 1.0f);
      ma_sound_set_volume(s, 1.0f);
      ma_sound_set_min_distance(s, 0.0f);
      ma_sound_set_max_distance(s, default_sound_max_distance);
    }
  }

  if (this->decode_frames_per_update == 0) {
    this->decode_frames_per_update = playback_sample_rate;
  }

  constexpr size_t cache_size = pcm_samples_to_bytes(48000, 8, format::f32) / sizeof(uint32_t);
  cache1.resize(cache_size, 0);
  cache2.resize(cache_size, 0);

  if (mixing_ == mixing_mode::owner) {
    output_ = std::make_unique<playback_stream>();
    const auto capacity = uint32_t(std::ceil(playback_sample_rate * config.output_buffer_seconds));
    result = ma_pcm_rb_init(ma_format_f32, playback_channels, capacity, nullptr, nullptr, &output_->ring);
    if (result != MA_SUCCESS) utils::error{}("sound: could not initialize {}-frame output ring: {}", capacity, int(result));
    output_->initialized = true;
  }
  if (device_initialized_) {
    result = ma_device_start(m_device.get());
    if (result != MA_SUCCESS) utils::error{}("Could not start sound device: {}", int(result));
  }
}

system::~system() noexcept { shutdown(); }

void system::shutdown() noexcept {
  if (device_initialized_) ma_device_stop(m_device.get());
  output_.reset();

  for (auto& ptr : m_instances) {
    uninit_sound_instance(ptr.get());
  }

  for (auto& ptr : m_instances_mono) {
    uninit_sound_instance(ptr.get());
  }

  for (auto& ptr : m_instances_stereo) {
    uninit_sound_instance(ptr.get());
  }

  m_tasks.clear();
  m_instances.clear();
  m_instances_mono.clear();
  m_instances_stereo.clear();
  if (engine_initialized_) { ma_engine_uninit(m_engine.get()); engine_initialized_ = false; }
  if (resource_manager_initialized_) { ma_resource_manager_uninit(m_resource_manager.get()); resource_manager_initialized_ = false; }
  if (device_initialized_) { ma_device_uninit(m_device.get()); device_initialized_ = false; }
  if (context_initialized_) { ma_context_uninit(m_context.get()); context_initialized_ = false; }
}

uint32_t system::resource_manager_worker_count() const noexcept {
  return resource_manager_initialized_ ? m_resource_manager->config.jobThreadCount : 0;
}
uint32_t system::native_device_count() const noexcept {
  const auto* engine_device = engine_initialized_ ? ma_engine_get_device(m_engine.get()) : nullptr;
  return uint32_t(device_initialized_) + uint32_t(engine_device != nullptr && engine_device != m_device.get());
}

void system::render(std::span<float> interleaved) {
  if (!offline_) utils::error{}("sound: explicit render is only available in offline mode");
  if (interleaved.size() % playback_channels != 0) utils::error{}("sound: {} samples are not whole {}-channel frames", interleaved.size(), playback_channels);
  size_t begin = 0;
  const size_t total = interleaved.size() / playback_channels;
  while (begin < total) {
    const auto count = uint32_t(std::min<size_t>(total - begin, UINT32_MAX));
    read_output(interleaved.data() + begin * playback_channels, count);
    begin += count;
  }
}

bool system::setup_sound(const struct task& task) {
  // еще нужно поглядеть будем ли мы вообще воспроизводить этот звук
  if (task.id == SIZE_MAX || task.res.data.empty() || task.res.type == data_type::undefined) {
    return false;
  }
  if (is_too_far(m_engine.get(), task)) {
    return false;
  }
  if (find_task_id(task.id) != SIZE_MAX) {
    return false;
  }
  // inst, task, timestamp, active_time, lifetime_budget, frames_decoded, stream_begin, stream_frames,
  // source_frames, phase, started, decoder, converter
  m_tasks.push_back({nullptr, task, cur_time, 0, 0, 0, 0, 0, 0, 0, task_phase::waiting_voice, false, nullptr, nullptr});
  return true;
}

bool system::remove_sound(const size_t task_id) {
  const size_t index = find_task_id(task_id);
  if (index == SIZE_MAX) {
    return false;
  }

  // единый путь освобождения: release_task вернёт/выгрузит голос (только если не разделён)
  // и освободит декодер/конвертер. Задачи, ДЕЛЯЩИЕ этот голос (after-цепочка), тоже снимаем.
  auto* inst = m_tasks[index].inst;
  if (inst != nullptr) {
    for (auto& t : m_tasks) {
      if (t.inst == inst) {
        t.decoder.reset(), t.converter.reset();
      }
    }
    ma_sound_stop(&inst->sound);
    inst->data_source.reset_stream();
    if (inst->pool == sound_instance::pool_kind::mono) {
      m_instances_mono_stack.push_back(inst);
    } else if (inst->pool == sound_instance::pool_kind::stereo) {
      m_instances_stereo_stack.push_back(inst);
    } else {
      uninit_sound_instance(inst);
      auto itr = std::find_if(m_instances.begin(), m_instances.end(), [inst](const auto& a) {
        return a.get() == inst;
      });
      if (itr != m_instances.end()) {
        m_instances.erase(itr);
      }
    }
    std::erase_if(m_tasks, [inst](const auto& t) {
      return t.inst == inst;
    });
    return true;
  }

  m_tasks.erase(m_tasks.begin() + index);
  return true;
}

bool system::play_sound(const size_t task_id) {
  const size_t index = find_task_id(task_id);
  if (index == SIZE_MAX) {
    return false;
  }

  auto& cur_task = m_tasks[index];
  if (cur_task.inst == nullptr) {
    return false;
  }
  const auto res = ma_sound_start(&cur_task.inst->sound);
  if (res == MA_SUCCESS) {
    cur_task.started = true;
  }
  return true;
}

bool system::stop_sound(const size_t task_id) {
  const size_t index = find_task_id(task_id);
  if (index == SIZE_MAX) {
    return false;
  }

  auto& cur_task = m_tasks[index];
  if (cur_task.inst == nullptr) {
    return false;
  }
  ma_sound_stop(&cur_task.inst->sound);
  cur_task.started = false;
  return true;
}

double system::stat_sound(const size_t task_id) const {
  const size_t index = find_task_id(task_id);
  if (index == SIZE_MAX) {
    return 0.0;
  }

  auto& cur_task = m_tasks[index];
  if (cur_task.inst == nullptr || cur_task.stream_frames_count == 0 || cur_task.source_frames_count == 0) {
    return cur_task.task.start; // ещё не играет → абсолютная стартовая позиция
  }

  // АБСОЛЮТНАЯ позиция в источнике (с учётом старта не с нуля), как и в make_status
  const size_t start_frame = cur_task.source_frames_count - cur_task.stream_frames_count;
  const size_t frames_read = cur_task.inst->data_source.frames_read_total.load(std::memory_order_acquire);
  if (frames_read <= cur_task.stream_begin_frame) {
    return cur_task.task.start;
  }
  const size_t local_frames = std::min(frames_read - cur_task.stream_begin_frame, cur_task.stream_frames_count);
  return double(start_frame + local_frames) / double(cur_task.source_frames_count);
}

task_status system::make_status(const system::sound_task& task) const {
  task_status status;
  status.id = task.task.id;
  status.resource_id = task.task.res.id;
  status.type = task.task.type;
  status.state = task_state::queued;
  // progress — АБСОЛЮТНАЯ позиция в ИСТОЧНИКЕ [0,1], а не доля проигранного сегмента.
  // При старте не с нуля сегмент = source - start_frame, поэтому без учёта start_frame
  // прогресс шёл бы 0→1 по куску и «вдвое быстрее». До старта показываем сам старт.
  status.progress = task.task.start;
  status.after = task.task.after;
  status.frames_decoded = task.frames_decoded;
  status.frames_total = task.stream_frames_count;
  status.underruns = 0;
  status.pos = task.task.pos;
  status.dir = task.task.dir;
  status.vel = task.task.vel;

  if (task.inst == nullptr || task.stream_frames_count == 0 || task.source_frames_count == 0) {
    return status;
  }

  const size_t start_frame = task.source_frames_count - task.stream_frames_count; // кадры до сегмента
  const size_t frames_read = task.inst->data_source.frames_read_total.load(std::memory_order_acquire);
  if (frames_read > task.stream_begin_frame) {
    const size_t local_frames = std::min(frames_read - task.stream_begin_frame, task.stream_frames_count);
    status.progress = double(start_frame + local_frames) / double(task.source_frames_count);
  }

  status.state = status.progress >= 1.0 ? task_state::finished : task_state::playing;
  status.underruns = task.inst->data_source.underrun_count.load(std::memory_order_acquire);
  return status;
}

bool system::stat_sound(const size_t task_id, struct task_status& out) const {
  const size_t index = find_task_id(task_id);
  if (index == SIZE_MAX) {
    return false;
  }
  out = make_status(m_tasks[index]);
  return true;
}

void system::snapshot(std::vector<struct task_status>& out) const {
  out.clear();
  out.reserve(m_tasks.size());
  for (const auto& task : m_tasks) {
    out.push_back(make_status(task));
  }
}

bool system::set_sound(const size_t task_id, const double place) {
  const size_t index = find_task_id(task_id);
  if (index == SIZE_MAX) {
    return false;
  }

  auto& cur_task = m_tasks[index];
  cur_task.task.start = std::clamp(place, 0.0, 1.0);
  if (cur_task.inst == nullptr) {
    return true;
  }

  const auto shared_inst = cur_task.inst;
  const auto shared_count = std::count_if(m_tasks.begin(), m_tasks.end(), [shared_inst](const auto& t) {
    return t.inst == shared_inst;
  });
  if (shared_count != 1) {
    return false;
  }

  ma_sound_stop(&shared_inst->sound);
  shared_inst->data_source.reset_stream();
  cur_task.frames_decoded = 0;
  cur_task.stream_begin_frame = 0;
  cur_task.output_frames_written = 0;
  cur_task.stream_frames_count = 0;
  cur_task.source_frames_count = 0;
  // назад в waiting_voice: голос ОСТАВЛЯЕМ (acquire_voice idempotent), init_decode пересоздаст
  // декодер с новой стартовой позицией. started сбрасываем — ma_sound_start снова в active.
  cur_task.phase = task_phase::waiting_voice;
  cur_task.started = false;
  cur_task.decoder.reset();
  cur_task.converter.reset();
  cur_task.timestamp = cur_time;
  return true;
}

bool system::update_sound(const struct task_update& task) {
  const size_t index = find_task_id(task.id);
  if (index == SIZE_MAX) {
    return false;
  }

  auto& cur_task = m_tasks[index];
  if (cur_task.inst == nullptr) {
    return false;
  }

  cur_task.task.pos = task.pos;
  cur_task.task.dir = task.dir;
  cur_task.task.vel = task.vel;

  if (is_too_far(m_engine.get(), cur_task.task)) {
    return remove_sound(task.id);
  }

  ma_sound_set_position(&cur_task.inst->sound, task.pos.x, task.pos.y, task.pos.z);
  ma_sound_set_direction(&cur_task.inst->sound, task.dir.x, task.dir.y, task.dir.z);
  ma_sound_set_velocity(&cur_task.inst->sound, task.vel.x, task.vel.y, task.vel.z);
  update_directional_coloration(cur_task);
  return true;
}

void system::set_decode_budget_frames(const size_t frames) noexcept {
  decode_frames_per_update = frames == 0 ? playback_sample_rate : frames;
}

bool system::set_listener_pos(const vec3& pos) {
  ma_engine_listener_set_position(m_engine.get(), 0, pos.x, pos.y, pos.z);
  for (auto& task : m_tasks) {
    update_directional_coloration(task);
  }
  return true;
}

bool system::set_listener_ori(const vec3& look_at, const vec3& up) {
  const auto pos = ma_engine_listener_get_position(m_engine.get(), 0);
  const auto norm = normalize(look_at - vec3(pos.x, pos.y, pos.z));
  ma_engine_listener_set_direction(m_engine.get(), 0, norm.x, norm.y, norm.z);
  ma_engine_listener_set_world_up(m_engine.get(), 0, up.x, up.y, up.z);
  for (auto& task : m_tasks) {
    update_directional_coloration(task);
  }
  return true;
}

bool system::set_listener_vel(const vec3& vel) {
  ma_engine_listener_set_velocity(m_engine.get(), 0, vel.x, vel.y, vel.z);
  return true;
}

// 1 - очень громко, наверное по умолчанию нужно ставить 0.2
// хотя может быть вообще замаппить [0, 0.2]
void system::set_master_volume(const float val) {
  ma_engine_set_volume(m_engine.get(), val);
}

void system::set_source_volume(const uint32_t type_index, const float val) {
  if (type_index >= source_volume.size()) {
    utils::warn("sound: source volume type {} is out of range", type_index);
    return;
  }
  source_volume[type_index] = std::clamp(val, 0.0f, 1.0f);
  for (auto& task : m_tasks) {
    if (task.inst == nullptr || static_cast<uint32_t>(task.task.type) != type_index) {
      continue;
    }
    ma_sound_set_volume(
      &task.inst->sound,
      task.task.volume * source_volume[type_index]);
  }
}

void system::set_directional_coloration(const directional_coloration_config& config) noexcept {
  directional_coloration_config_ = sanitize_directional_coloration_config(config);
  for (auto& task : m_tasks) {
    update_directional_coloration(task);
  }
}

const directional_coloration_config& system::directional_coloration() const noexcept {
  return directional_coloration_config_;
}

void system::update_directional_coloration(sound_task& task) noexcept {
  if (task.inst == nullptr || !task.inst->coloration_initialized) {
    return;
  }

  float target_db = 0.0f;
  if (task.task.max_distance > 0.0f && directional_coloration_config_.enabled) {
    const auto listener_pos = ma_engine_listener_get_position(m_engine.get(), 0);
    const auto listener_forward = ma_engine_listener_get_direction(m_engine.get(), 0);
    const auto listener_up = ma_engine_listener_get_world_up(m_engine.get(), 0);
    const auto response = compute_directional_coloration(
      directional_coloration_config_,
      vec3(listener_pos.x, listener_pos.y, listener_pos.z),
      vec3(listener_forward.x, listener_forward.y, listener_forward.z),
      vec3(listener_up.x, listener_up.y, listener_up.z),
      task.task.pos);
    target_db = response.high_shelf_db;
  }

  directional_coloration_node_set(
    &task.inst->coloration,
    directional_coloration_config_,
    target_db);
}

// Выдать голос под задачу. idempotent: если голос уже есть (в т.ч. разделённый с prev
// для after) — просто true. Для after берём голос prev (gapless). Пул пуст → false (ждём).
bool system::acquire_voice(sound_task& t) {
  if (t.inst != nullptr) {
    return true;
  }

  const auto pop = [](std::vector<sound_instance*>& stack) -> sound_instance* {
    if (stack.empty()) {
      return nullptr;
    }
    auto* inst = stack.back();
    stack.pop_back();
    inst->data_source.reset_stream();
    return inst;
  };

  if (t.task.after != SIZE_MAX) {
    const size_t prev_index = find_task_id(t.task.after);
    if (prev_index == SIZE_MAX) {
      return false; // цепочка оборвана — дропнется по таймауту
    }
    auto* prev = m_tasks[prev_index].inst;
    if (prev == nullptr) {
      return false; // prev ещё не получил голос — ждём
    }
    t.inst = prev; // gapless: делим голос prev
    return true;
  }

  if (t.task.type == type::sfx || t.task.type == type::talk_pos) {
    t.inst = pop(m_instances_mono_stack);
  } else if (t.task.type == type::ui_effect) {
    t.inst = pop(m_instances_stereo_stack);
  } else {
    // неизвестный тип: нужен декодер, чтобы узнать число каналов (не pool-starved путь)
    init_decode(t);
    if (t.decoder->channels() == 1) {
      t.inst = pop(m_instances_mono_stack);
    } else if (t.decoder->channels() == 2) {
      t.inst = pop(m_instances_stereo_stack);
    } else {
      m_instances.emplace_back(new sound_instance);
      instance_init(m_instances.back().get(), playback_sample_rate, t.decoder->channels(), format::f32);
      t.inst = m_instances.back().get();
      t.inst->data_source.reset_stream();
    }
  }
  return t.inst != nullptr;
}

// Создать декодер + конвертер и посчитать frames сегмента. idempotent. Тяжёлые ресурсы
// (в т.ч. КОПИЯ PCM в pcm_decoder) — создаём ТОЛЬКО отсюда, на входе в active (после голоса).
void system::init_decode(sound_task& t) {
  if (t.decoder) {
    return;
  }

  // PCM-ветка: данные уже декодированы (короткий звук), метаданные в resource2 → passthrough.
  if (t.task.res.type == data_type::pcm) {
    const auto& r = t.task.res;
    t.decoder = std::make_unique<pcm_decoder>(
      r.data.data(), r.data.size(), r.sample_format, r.channels, r.sample_rate, r.frames_count);
  } else {
    t.decoder = make_decoder(t.task.res.type, t.task.res.id, t.task.res.data);
  }
  if (!t.decoder) {
    utils::error{}("Could not create decoder for sound task '{}'", t.task.res.id);
  }

  t.source_frames_count = t.decoder->frames_count();
  const size_t start_frame = std::min<size_t>(t.task.start * double(t.source_frames_count), t.source_frames_count);
  if (start_frame != 0 && !t.decoder->seek(start_frame)) {
    utils::error{}("Could not seek sound task '{}' to frame {}", t.task.res.id, start_frame);
  }
  t.stream_frames_count = t.source_frames_count - start_frame;

  const uint32_t out_channels =
    (t.task.type == type::sfx || t.task.type == type::talk_pos) ? 1 : (t.task.type == type::ui_effect ? 2 : t.decoder->channels());

  auto cfg = ma_data_converter_config_init(
    static_cast<ma_format>(t.decoder->format()), ma_format_f32,
    t.decoder->channels(), out_channels, t.decoder->sample_rate(), playback_sample_rate);
  t.converter.reset(new miniaudio_data_converter);
  auto res = ma_data_converter_init(&cfg, nullptr, t.converter.get());
  if (res != MA_SUCCESS) {
    utils::error{}("Could not initialize data converter for sound task '{}'", t.task.res.id);
  }
  t.converter->initialized = true;
}

// Терминал: единая точка очистки для ЛЮБОГО завершения (dropped/доиграл/дедлайн). Голос
// возвращается в пул / выгружается только если больше НИКТО его не держит (after-цепочка).
void system::release_task(sound_task& t) {
  auto* inst = t.inst;
  if (inst != nullptr) {
    const bool shared = std::any_of(m_tasks.begin(), m_tasks.end(),
                                    [inst, &t](const auto& o) {
                                      return &o != &t && o.inst == inst;
                                    });
    if (!shared) {
      ma_sound_stop(&inst->sound);
      inst->data_source.reset_stream();
      if (inst->pool == sound_instance::pool_kind::mono) {
        m_instances_mono_stack.push_back(inst);
      } else if (inst->pool == sound_instance::pool_kind::stereo) {
        m_instances_stereo_stack.push_back(inst);
      } else {
        uninit_sound_instance(inst);
        auto it = std::find_if(m_instances.begin(), m_instances.end(),
                               [inst](const auto& p) {
                                 return p.get() == inst;
                               });
        if (it != m_instances.end()) {
          m_instances.erase(it);
        }
      }
    }
    t.inst = nullptr;
  }
  t.decoder.reset(); // освобождаем тяжёлые ресурсы (в т.ч. копию PCM)
  t.converter.reset();
}

void system::update(const size_t time) {
  this->cur_time += time;

  // Гарантии терминации (лекарство от утечки задач/голосов):
  constexpr size_t voice_wait_timeout = utils::global_time_resolution / 10; // 100 мс без голоса → dropped (fire-and-forget)
  constexpr size_t lifetime_margin = utils::global_time_resolution;         // +1 с к длительности сегмента — страховочный дедлайн жизни

  for (auto& t : m_tasks) {
    switch (t.phase) {
      case task_phase::waiting_voice: {
        if (!acquire_voice(t)) {
          // не смогли начать. after, ждущий ЖИВОГО prev, не дропаем (у prev свой дедлайн);
          // остальные fire-and-forget — по таймауту, иначе безголосый backlog копится вечно.
          const bool waiting_live_after =
            (t.task.after != SIZE_MAX) && (find_task_id(t.task.after) != SIZE_MAX);
          if (!waiting_live_after && cur_time - t.timestamp > voice_wait_timeout) {
            t.phase = task_phase::finished; // dropped
          }
          break;
        }

        init_decode(t);                                                          // тяжёлые ресурсы — только теперь (голос есть)
        ensure_stream_buffer(&t.inst->data_source, this->stream_buffer_seconds); // ленивое кольцо

        // after: ждём, пока prev полностью декодирует свой сегмент (gapless на общем голосе)
        if (t.task.after != SIZE_MAX) {
          const size_t prev_index = find_task_id(t.task.after);
          if (prev_index != SIZE_MAX) {
            const auto& prev = m_tasks[prev_index];
            if (prev.phase == task_phase::waiting_voice || prev.frames_decoded < prev.stream_frames_count) {
              break;
            }
          }
        }

        const auto type_index = static_cast<size_t>(t.task.type);
        const float category_gain = type_index < source_volume.size() ? source_volume[type_index] : 1.0f;
        ma_sound_set_volume(&t.inst->sound, t.task.volume * category_gain);
        ma_sound_set_pitch(&t.inst->sound, t.task.pitch);
        // Позиционность — свойство ЗАДАЧИ, а голос переиспользуется: выставляем ОБА направления
        // на каждой выдаче, иначе прошлый жилец голоса протечёт в текущего.
        if (t.task.max_distance > 0.0f) {
          ma_sound_set_spatialization_enabled(&t.inst->sound, MA_TRUE);
          ma_sound_set_min_distance(&t.inst->sound, std::max(t.task.min_distance, 0.0f));
          ma_sound_set_max_distance(&t.inst->sound, t.task.max_distance);
        } else {
          ma_sound_set_spatialization_enabled(&t.inst->sound, MA_FALSE);
        }
        ma_sound_set_position(&t.inst->sound, t.task.pos.x, t.task.pos.y, t.task.pos.z);
        ma_sound_set_direction(&t.inst->sound, t.task.dir.x, t.task.dir.y, t.task.dir.z);
        ma_sound_set_velocity(&t.inst->sound, t.task.vel.x, t.task.vel.y, t.task.vel.z);
        update_directional_coloration(t);

        t.stream_begin_frame = t.inst->data_source.frames_written_total.load(std::memory_order_acquire);
        const size_t rate = t.decoder->sample_rate() != 0 ? t.decoder->sample_rate() : playback_sample_rate;
        t.active_time = cur_time;
        t.lifetime_budget = size_t(uint64_t(t.stream_frames_count) * utils::global_time_resolution / std::max<size_t>(rate, 1)) + lifetime_margin;
        t.phase = task_phase::active;
        [[fallthrough]]; // можно начать декод в этот же тик
      }
      case task_phase::active: {
        if (cur_time - t.active_time > t.lifetime_budget) {
          t.phase = task_phase::finished;
          break;
        } // страховка

        const bool was_start = t.frames_decoded == 0;
        const size_t free_frames = t.inst->data_source.available_frames_to_write();
        if (free_frames > 0 && t.frames_decoded < t.stream_frames_count) {
          const size_t input_cache_frames = bytes_to_pcm_frames(cache1.size() * sizeof(cache1[0]), t.decoder->channels(), t.decoder->format());
          const size_t output_cache_frames = bytes_to_pcm_frames(cache2.size() * sizeof(cache2[0]), t.inst->data_source.channels, t.inst->data_source.format);
          const size_t output_capacity = std::min(free_frames, output_cache_frames);
          ma_uint64 required_input = 0;
          const auto estimate = ma_data_converter_get_required_input_frame_count(t.converter.get(), output_capacity, &required_input);
          if (estimate != MA_SUCCESS) utils::error{}("sound task '{}': could not estimate converter input: {}", t.task.res.id, int(estimate));
          const size_t frames_to_read = std::min({size_t(required_input), decode_frames_per_update, input_cache_frames,
                                                  t.stream_frames_count - t.frames_decoded});
          if (frames_to_read > 0) {
            ma_uint64 frames = t.decoder->get_frames(cache1.data(), frames_to_read);
            if (frames == 0) {
              // декодер исчерпан раньше оценки stream_frames_count (у сжатых форматов
              // frames_count() — оценка) → фиксируем реально произведённое, иначе не завершится.
              t.stream_frames_count = t.frames_decoded;
            } else {
              ma_uint64 frames_out = output_capacity;
              const ma_uint64 read = frames;
              auto res = ma_data_converter_process_pcm_frames(t.converter.get(), cache1.data(), &frames, cache2.data(), &frames_out);
              if (res != MA_SUCCESS) {
                utils::error{}("Could not convert {} frames data for resorce '{}'", frames, t.task.res.id);
              }
              if (frames != read) utils::error{}("sound task '{}': converter consumed {} of {} input frames", t.task.res.id, frames, read);
              t.frames_decoded += frames;
              t.output_frames_written += frames_out;
              const size_t writed_frames = write_decoded_pcm_frames(&t.inst->data_source, cache2.data(), frames_out);
              assert(writed_frames == frames_out);
              static_cast<void>(writed_frames);
            }
          }
        }

        if (was_start && !t.started) {
          const auto res = ma_sound_start(&t.inst->sound);
          assert(res == MA_SUCCESS);
          static_cast<void>(res);
          t.started = true;
        }

        if (t.frames_decoded >= t.stream_frames_count) {
          t.phase = task_phase::draining;
        }
        break;
      }
      case task_phase::draining: {
        // штатно: кольцо вычитано callback'ом. Дедлайн — страховка от «неотыгранных» голосов
        // (позиционный/приглушённый звук, который miniaudio может перестать читать).
        const size_t frames_read = t.inst->data_source.frames_read_total.load(std::memory_order_acquire);
        const bool fully_read = frames_read >= t.stream_begin_frame + t.output_frames_written;
        if (fully_read || cur_time - t.active_time > t.lifetime_budget) {
          t.phase = task_phase::finished;
        }
        break;
      }
      case task_phase::finished: break;
    }
  }

  // ЕДИНАЯ точка очистки: любой finished → вернуть голос + освободить ресурсы + снять задачу.
  for (auto it = m_tasks.begin(); it != m_tasks.end();) {
    if (it->phase == task_phase::finished) {
      release_task(*it);
      it = m_tasks.erase(it);
    } else {
      ++it;
    }
  }
  if (output_) mix_output();
}

size_t system::find_task_id(const size_t task_id) const {
  if (task_id == SIZE_MAX) {
    return SIZE_MAX;
  }

  const auto itr = std::find_if(m_tasks.begin(), m_tasks.end(), [task_id](const auto& inst) {
    return inst.task.id == task_id;
  });
  if (itr != m_tasks.end() && itr->task.id == task_id) {
    return itr - m_tasks.begin();
  }
  return SIZE_MAX;
}

// останавливать ma_device когда создаем/удаляем инстанс?
// похоже что это пока что костыль, но что то подобное делать придется
uint32_t system::instance_init(sound_instance* inst, const uint32_t sample_rate, const uint32_t channels, const enum format format) const {
  auto s = &inst->sound;
  auto ds = &inst->data_source;

  auto result = standart_source_init(ds, sample_rate, channels, format, stream_buffer_seconds);
  if (result != MA_SUCCESS) {
    utils::error{}("Could not initialize data source");
  }

  inst->data_source_initialized = true;
  uint32_t flags = 0;
  if (channels > 1) {
    flags = MA_SOUND_FLAG_NO_SPATIALIZATION;
  }

  // нужно заранее продумать звуковые группы
  result = ma_sound_init_from_data_source(m_engine.get(), ds, flags, nullptr, s);
  if (result != MA_SUCCESS) {
    utils::error{}("Could not initialize sound");
  }
  inst->sound_initialized = true;
  if (channels == 1) {
    result = attach_directional_coloration(m_engine.get(), inst);
    if (result != MA_SUCCESS) {
      utils::error{}("Could not attach directional coloration node");
    }
  }

  ma_sound_set_attenuation_model(s, ma_attenuation_model_linear);
  ma_sound_set_rolloff(s, 1.0f);
  ma_sound_set_min_gain(s, 0.0f);
  ma_sound_set_max_gain(s, 1.0f);
  ma_sound_set_min_distance(s, 0.0f);
  ma_sound_set_max_distance(s, default_sound_max_distance);

  return MA_SUCCESS;
}
} // namespace sound
} // namespace devils_engine
