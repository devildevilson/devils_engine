#ifndef DEVILS_ENGINE_SOUND_MP3_DECODER_H
#define DEVILS_ENGINE_SOUND_MP3_DECODER_H

#include <vector>
#include <string_view>

#include "decoder.h"
#include "dr_mp3.h"

namespace devils_engine {
  namespace sound {
    class mp3_decoder : public decoder {
    public:
      mp3_decoder(const std::string_view &name, const void* memory, const size_t memory_size);
      ~mp3_decoder() noexcept;

      bool seek(const size_t seek_size) override;
      size_t get_frames(void* memory, const size_t frames_count, const uint16_t channels_override = 0) override;
      size_t get_frames(
        const uint32_t al_buffer,
        const size_t frames_count,
        const uint16_t channels_override = 0,
        const uint32_t sample_rate_override = 0
      ) override;
    private:
      drmp3 data;
      std::vector<uint8_t> buffer;
    };
  }
}

#endif
