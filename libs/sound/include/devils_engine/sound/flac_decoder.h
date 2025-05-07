#ifndef DEVILS_ENGINE_SOUND_FLAC_DECODER_H
#define DEVILS_ENGINE_SOUND_FLAC_DECODER_H

#include <vector>
#include <string_view>

#include "decoder.h"
#include "dr_flac.h"

namespace devils_engine {
  namespace sound {
    class flac_decoder : public decoder {
    public:
      flac_decoder(const std::string_view &name, const void* memory, const size_t memory_size);
      ~flac_decoder() noexcept;

      bool seek(const size_t seek_size) override;
      size_t get_frames(void* memory, const size_t frames_count, const uint16_t channels_override = 0) override;
      size_t get_frames(
        const uint32_t al_buffer,
        const size_t frames_count,
        const uint16_t channels_override = 0,
        const uint32_t sample_rate_override = 0
      ) override;
    private:
      drflac* data;
      std::vector<uint8_t> buffer;
    };
  }
}

#endif
