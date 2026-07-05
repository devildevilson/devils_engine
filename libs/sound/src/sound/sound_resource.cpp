#include "sound_resource.h"

#include <string_view>

#include <devils_engine/utils/core.h>
#include <devils_engine/utils/safe_handle.h>
#include <devils_engine/demiurg/module_interface.h>

#include "decoder.h"  // make_decoder + интерфейс decoder (метаданные + get_frames)
#include "common.h"   // format helpers (pcm_frame_to_bytes и т.п.)

namespace devils_engine {
namespace sound {

static data_type type_from_ext(const std::string_view ext) noexcept {
  if (ext == "mp3")  return data_type::mp3;
  if (ext == "flac") return data_type::flac;
  if (ext == "wav")  return data_type::wav;
  if (ext == "ogg")  return data_type::ogg;
  // pcm НЕ поддержан в resource2/system2 пути: make_decoder(pcm) отдаёт null → play был бы фатальным.
  // Не заявляем '.pcm' как загружаемый формат, пока PCM-ветка не готова (см. system.cpp make_decoder).
  return data_type::undefined;
}

sound_resource::sound_resource() {
  set_flag(demiurg::resource_flags::warm_and_hot_same, true); // CPU-only: warm == hot
  set_flag(demiurg::resource_flags::binary, true);
}

// порог "короткого" звука — декодируем такие целиком в PCM (микшер не декодит их per-frame)
static constexpr double small_sound_seconds = 5.0;

void sound_resource::load_cold(const utils::safe_handle_t&) {
  module->load_binary(path, data);
  type = type_from_ext(ext);
  if (type == data_type::undefined) {
    utils::warn("sound_resource '{}': unknown extension '{}'", id, ext);
    return;
  }

  // Метаданные исходного аудио заполняем ВСЕГДА: создаём временный декодер по сжатым байтам и
  // считываем format/channels/sample_rate/frames_count (полезно и demiurg::resource, и микшеру).
  auto dec = make_decoder(type, id, std::span<const char>(data));
  if (!dec) {
    utils::warn("sound_resource '{}': no decoder for type {}", id, size_t(type));
    return;
  }
  sample_format = dec->format();
  channels      = dec->channels();
  sample_rate   = dec->sample_rate();
  frames_count  = dec->frames_count();

  // Короткие звуки декодируем целиком в PCM: data заменяем на сырые кадры, type=pcm. Микшер тогда
  // играет их через PCM-ветку (pcm_decoder — passthrough), без per-frame декода сжатых данных.
  const size_t small_frames = static_cast<size_t>(small_sound_seconds * double(sample_rate));
  const bool convertible = frames_count > 0 && channels > 0 && sample_format != format::unknown;
  if (convertible && frames_count < small_frames) {
    const size_t frame_bytes = pcm_frame_to_bytes(channels, sample_format);
    std::vector<char> pcm(frames_count * frame_bytes, 0);
    size_t got = 0;
    while (got < frames_count) {
      const size_t n = dec->get_frames(pcm.data() + got * frame_bytes, frames_count - got);
      if (n == 0) break; // декодер исчерпан (реальное число кадров могло быть меньше заявленного)
      got += n;
    }
    pcm.resize(got * frame_bytes);
    data = std::move(pcm);
    frames_count = got;
    type = data_type::pcm;
    utils::info("sound_resource '{}': decoded to PCM ({} frames, {} ch, {} Hz, fmt {}, {} bytes)",
      id, frames_count, channels, sample_rate, size_t(sample_format), data.size());
  } else {
    utils::info("sound_resource '{}': loaded {} bytes (type {}, {} frames, {} ch, {} Hz)",
      id, data.size(), size_t(type), frames_count, channels, sample_rate);
  }
}

void sound_resource::load_warm(const utils::safe_handle_t&) {}
void sound_resource::unload_hot(const utils::safe_handle_t&) {}
void sound_resource::unload_warm(const utils::safe_handle_t&) { data.clear(); data.shrink_to_fit(); }

}
}
