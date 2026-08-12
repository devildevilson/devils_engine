#include <cstring>

#include "devils_engine/utils/core.h"
#include "pcm_decoder.h"

namespace devils_engine {
namespace sound {
pcm_decoder::pcm_decoder(
  const void* memory,
  const size_t memory_size,
  const enum format format,
  const uint16_t channels,
  const uint32_t sample_rate,
  const size_t frames_count) : current_index(0), buffer(memory_size, 0) {
  memcpy(buffer.data(), memory, memory_size);
  m_channels = channels;
  m_format = format;
  m_sample_rate = sample_rate;
  m_frames_count = frames_count;
}

pcm_decoder::pcm_decoder(decoder* parent) : current_index(0),
                                            buffer(pcm_samples_to_bytes(parent->frames_count(), parent->channels(), parent->format()), 0) {
  m_channels = parent->channels();
  m_format = parent->format();
  m_sample_rate = parent->sample_rate();
  m_frames_count = parent->frames_count();
  if (!parent->seek(0)) {
    utils::error{}("Could not initialize pcm_decoder");
  }
  parent->get_frames(buffer.data(), parent->frames_count());
}

pcm_decoder::~pcm_decoder() noexcept {}

bool pcm_decoder::seek(const size_t seek_size) {
  current_index = pcm_samples_to_bytes(seek_size, channels(), format());
  current_index = std::min(current_index, buffer.size());
  return true;
}

size_t pcm_decoder::get_frames(void* memory, const size_t frames_count, const uint16_t channels_override) {
  const uint16_t final_channels = channels_override != 0 ? channels_override : channels();

  const size_t expected_bytes = pcm_samples_to_bytes(frames_count, channels(), format());
  const size_t bytes_size = std::min(expected_bytes, buffer.size() - current_index);
  if (bytes_size == 0) {
    return 0;
  }

  size_t readed_frames = 0;
  if (final_channels == 1 && channels() != 1) {
    readed_frames = bytes_to_pcm_frames(bytes_size, channels(), format());
    if (format() == format::u8) {
      make_mono(reinterpret_cast<uint8_t*>(memory), &buffer[current_index], readed_frames, channels());
    } else if (format() == format::s16) {
      make_mono(reinterpret_cast<int16_t*>(memory), reinterpret_cast<int16_t*>(&buffer[current_index]), readed_frames, channels());
    } else if (format() == format::f32) {
      make_mono(reinterpret_cast<float*>(memory), reinterpret_cast<float*>(&buffer[current_index]), readed_frames, channels());
    }
    current_index += bytes_size;
  } else if (final_channels == channels()) {
    readed_frames = bytes_to_pcm_frames(bytes_size, final_channels, format());
    memcpy(memory, &buffer[current_index], bytes_size);
    current_index += bytes_size;
  } else {
  }

  return readed_frames;
}
} // namespace sound
} // namespace devils_engine
