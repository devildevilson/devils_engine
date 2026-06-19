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
      inline decoder() noexcept :
        m_format(format::unknown),
        m_channels(0),
        m_sample_rate(0),
        m_frames_count(0)
      {}

      virtual ~decoder() noexcept = default;

      virtual bool seek(const size_t seek_size) = 0;
      virtual size_t get_frames(void* memory, const size_t frames_count, const uint16_t channels_override = 0) = 0;
      // нужна ли мне эта функция? скорее нет чем да, но пока оставим
      virtual size_t get_frames(
        const uint32_t al_buffer,
        const size_t frames_count,
        const uint16_t channels_override = 0,
        const uint32_t sample_rate_override = 0
      ) = 0;

      inline enum format format() const noexcept { return m_format; }
      inline uint16_t channels() const noexcept { return m_channels; }
      inline uint32_t sample_rate() const noexcept { return m_sample_rate; }
      inline size_t frames_count() const noexcept { return m_frames_count; }
    protected:
      enum format m_format;
      uint16_t m_channels;
      uint32_t m_sample_rate;
      size_t m_frames_count;
    };

    std::unique_ptr<decoder> make_decoder(const data_type type, const std::string_view &name, const std::span<const char> &data);
  }
}

#endif
