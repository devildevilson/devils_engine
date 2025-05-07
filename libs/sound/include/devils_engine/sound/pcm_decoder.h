#ifndef DEVILS_ENGINE_SOUND_PCM_DECODER_H
#define DEVILS_ENGINE_SOUND_PCM_DECODER_H

#include <vector>
#include <string_view>

#include "decoder.h"

namespace devils_engine {
  namespace sound {
    class pcm_decoder : public decoder {
    public:
      pcm_decoder(
        const void* memory,
        const size_t memory_size,
        const uint16_t channels,
        const uint32_t bits_pes_channel,
        const uint32_t sample_rate,
        const size_t frames_count
      );

      pcm_decoder(decoder* parent);
      ~pcm_decoder() noexcept;

      bool seek(const size_t seek_size) override;
      size_t get_frames(void* memory, const size_t frames_count, const uint16_t channels_override = 0) override;
      size_t get_frames(
        const uint32_t al_buffer,
        const size_t frames_count,
        const uint16_t channels_override = 0,
        const uint32_t sample_rate_override = 0
      ) override;
    private:
      size_t current_index;
      std::vector<uint8_t> buffer;
    };
  }
}

#endif
