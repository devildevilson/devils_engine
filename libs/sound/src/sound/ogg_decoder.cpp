#include "devils_engine/utils/core.h"
#include "ogg_decoder.h"

#define STB_VORBIS_NO_STDIO
#include "stb_vorbis.c"

namespace devils_engine {
namespace sound {
ogg_decoder::ogg_decoder(const std::string_view& name, const void* memory, const size_t memory_size) {
  int32_t err;
  data = stb_vorbis_open_memory(reinterpret_cast<const uint8_t*>(memory), memory_size, &err, nullptr);
  if (data == nullptr) {
    utils::error{}("Failed to parse ogg resource '{}'", name);
  }

  const stb_vorbis_info& info = stb_vorbis_get_info(data);

  m_sample_rate = info.sample_rate;
  m_format = format::f32;
  m_channels = info.channels;
  // либо я путаю, либо здесь напортачили, но тут возвращаются фреймы (фрейм = сэмпл / канал)
  // а ниже в функциях stb_vorbis_get_samples_* возвращаются именно сэмплы
  m_frames_count = stb_vorbis_stream_length_in_samples(data); //  / m_channels
}

ogg_decoder::~ogg_decoder() noexcept {
  stb_vorbis_close(data);
}

bool ogg_decoder::seek(const size_t seek_size) {
  return stb_vorbis_seek_frame(data, seek_size);
}

size_t ogg_decoder::get_frames(void* memory, const size_t frames_count, const uint16_t channels_override) {
  const uint16_t final_channels = channels_override != 0 ? channels_override : channels();
  const size_t num_floats = frames_count * final_channels;
  const size_t readed_samples =
    stb_vorbis_get_samples_float_interleaved(data, final_channels, reinterpret_cast<float*>(memory), num_floats);
  return readed_samples / final_channels;
}
} // namespace sound
} // namespace devils_engine
