#include <string_view>

#include <devils_engine/catalogue/logging.h>
#include <devils_engine/demiurg/module_interface.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>

#include "common.h"  // format helpers (pcm_frame_to_bytes и т.п.)
#include "decoder.h" // make_decoder + интерфейс decoder (метаданные + get_frames)
#include "sound_resource.h"

namespace devils_engine {
namespace sound {

static data_type type_from_ext(const std::string_view ext) noexcept {
  if (ext == "mp3") {
    return data_type::mp3;
  }
  if (ext == "flac") {
    return data_type::flac;
  }
  if (ext == "wav") {
    return data_type::wav;
  }
  if (ext == "ogg") {
    return data_type::ogg;
  }
  if (ext == "opus") {
    return data_type::opus;
  }
  // pcm НЕ поддержан в resource2/system2 пути: make_decoder(pcm) отдаёт null → play был бы фатальным.
  // Не заявляем '.pcm' как загружаемый формат, пока PCM-ветка не готова (см. system.cpp make_decoder).
  return data_type::undefined;
}

sound_resource::sound_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true); // CPU-only: warm == hot
  set_flag(demiurg::resource_flags::binary, true);
}

resource2 sound_resource::view() const noexcept {
  return resource2(pin());
}

std::shared_ptr<const resource_blob> sound_resource::pin() const noexcept {
  return storage_.load(std::memory_order_acquire);
}

// порог "короткого" звука — декодируем такие целиком в PCM (микшер не декодит их per-frame)
static constexpr double small_sound_seconds = 5.0;

void sound_resource::load_cold(const utils::safe_handle_t&) {
  auto blob = std::make_shared<resource_blob>();
  blob->id = id;
  module->load_binary(path, blob->data);
  blob->type = type_from_ext(ext);
  if (blob->type == data_type::undefined) {
    utils::warn("sound_resource '{}': unknown extension '{}'", id, ext);
    storage_.store({}, std::memory_order_release);
    return;
  }

  // Метаданные исходного аудио заполняем ВСЕГДА: создаём временный декодер по сжатым байтам и
  // считываем format/channels/sample_rate/frames_count (полезно и demiurg::resource, и микшеру).
  auto dec = make_decoder(blob->type, id, std::span<const char>(blob->data));
  if (!dec) {
    utils::warn("sound_resource '{}': no decoder for type {}", id, size_t(blob->type));
    storage_.store({}, std::memory_order_release);
    return;
  }
  blob->sample_format = dec->format();
  blob->channels = dec->channels();
  blob->sample_rate = dec->sample_rate();
  blob->frames_count = dec->frames_count();

  // Короткие звуки декодируем целиком в PCM: data заменяем на сырые кадры, type=pcm. Микшер тогда
  // играет их через PCM-ветку (pcm_decoder — passthrough), без per-frame декода сжатых данных.
  const size_t small_frames = static_cast<size_t>(small_sound_seconds * double(blob->sample_rate));
  const bool convertible = blob->frames_count > 0 && blob->channels > 0 && blob->sample_format != format::unknown;
  if (convertible && blob->frames_count < small_frames) {
    const size_t frame_bytes = pcm_frame_to_bytes(blob->channels, blob->sample_format);
    std::vector<char> pcm(blob->frames_count * frame_bytes, 0);
    size_t got = 0;
    while (got < blob->frames_count) {
      const size_t n = dec->get_frames(pcm.data() + got * frame_bytes, blob->frames_count - got);
      if (n == 0) {
        break; // декодер исчерпан (реальное число кадров могло быть меньше заявленного)
      }
      got += n;
    }
    pcm.resize(got * frame_bytes);
    blob->data = std::move(pcm);
    blob->frames_count = got;
    blob->type = data_type::pcm;
    DE_LOG(catalogue::log_domain::resource, flow, "sound_resource '{}': decoded to PCM ({} frames, {} ch, {} Hz, fmt {}, {} bytes)",
           id, blob->frames_count, blob->channels, blob->sample_rate, size_t(blob->sample_format), blob->data.size());
  } else {
    DE_LOG(catalogue::log_domain::resource, flow, "sound_resource '{}': loaded {} bytes (type {}, {} frames, {} ch, {} Hz)",
           id, blob->data.size(), size_t(blob->type), blob->frames_count, blob->channels, blob->sample_rate);
  }

  storage_.store(std::move(blob), std::memory_order_release);
}

void sound_resource::load_warm(const utils::safe_handle_t&) {}
void sound_resource::unload_hot(const utils::safe_handle_t&) {}
void sound_resource::unload_warm(const utils::safe_handle_t&) {
  storage_.store({}, std::memory_order_release);
}

} // namespace sound
} // namespace devils_engine
