#ifndef DEVILS_ENGINE_SOUND_DECODER_H
#define DEVILS_ENGINE_SOUND_DECODER_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string_view>

#include "common.h"

namespace devils_engine {
namespace sound {
class decoder {
public:
  decoder() noexcept;

  virtual ~decoder() noexcept = default;

  virtual bool seek(const size_t seek_size) = 0;
  virtual size_t get_frames(void* memory, const size_t frames_count, const uint16_t channels_override = 0) = 0;
  // нужна ли мне эта функция? скорее нет чем да, но пока оставим
  virtual size_t get_frames(
    const uint32_t al_buffer,
    const size_t frames_count,
    const uint16_t channels_override = 0,
    const uint32_t sample_rate_override = 0) = 0;

  enum format format() const noexcept;
  uint16_t channels() const noexcept;
  uint32_t sample_rate() const noexcept;
  size_t frames_count() const noexcept;

protected:
  enum format m_format;
  uint16_t m_channels;
  uint32_t m_sample_rate;
  size_t m_frames_count;
};

std::unique_ptr<decoder> make_decoder(const data_type type, const std::string_view& name, const std::span<const char>& data);
} // namespace sound
} // namespace devils_engine

#endif
