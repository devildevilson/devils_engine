#include "flac_decoder.h"

#define DR_FLAC_NO_STDIO
#define DR_FLAC_IMPLEMENTATION
#include "dr_flac.h"

#include <cstdlib>
#include <stdexcept>

#include "AL/al.h"
#include "al_helper.h"

static void* my_malloc(size_t sz, void* pUserData) {
  (void)pUserData;
  return malloc(sz);
}

static void* my_realloc(void* p, size_t sz, void* pUserData) {
  (void)pUserData;
  return realloc(p, sz);
}

static void my_free(void* p, void* pUserData) {
  (void)pUserData;
  free(p);
}

namespace devils_engine {
  namespace sound {
    flac_decoder::flac_decoder(const std::string_view &name, const void* memory, const size_t memory_size) {
      const drflac_allocation_callbacks allocation_callbacks{
        nullptr,
        my_malloc,
        my_realloc,
        my_free
      };
      data = drflac_open_memory(memory, memory_size, &allocation_callbacks);
      if (data == nullptr) utils::error{}("Failed to parse flac resource '{}'", name);

      m_sample_rate = data->sampleRate;
      m_bits_per_channel = data->bitsPerSample;
      m_frames_count = data->totalPCMFrameCount;
      m_channels = data->channels;
    }

    flac_decoder::~flac_decoder() noexcept {
      drflac_close(data);
      data = nullptr;
    }

    bool flac_decoder::seek(const size_t seek_size) {
      return drflac_seek_to_pcm_frame(data, seek_size);
    }

    template <typename T>
    size_t get_frames_templ(
      drflac* data, std::vector<uint8_t> &buffer,
      void* memory, const size_t frames_count,
      const uint16_t channels
    ) {
      static_assert(std::is_same_v<T, float> || std::is_same_v<T, int16_t>, "Supported formats is float32 and signed16");

      auto* final_data = reinterpret_cast<T*>(memory);
      size_t readed_frames = 0;
      if (channels == 1 && data->channels != 1) {
        const size_t block_bytes_size = pcm_frames_to_bytes(frames_count, data->channels, data->bitsPerSample);
        if (buffer.size() < block_bytes_size) buffer.resize(block_bytes_size, 0);
        auto block_data = reinterpret_cast<T*>(buffer.data());

        if constexpr (std::is_same_v<T, float>) {
          readed_frames = drflac_read_pcm_frames_f32(data, frames_count, block_data);
        } else if constexpr (std::is_same_v<T, int16_t>) {
          readed_frames = drflac_read_pcm_frames_s16(data, frames_count, block_data);
        }

        make_mono(final_data, block_data, readed_frames, data->channels);

      } else if (channels == data->channels) {
        if constexpr (std::is_same_v<T, float>) {
          readed_frames = drflac_read_pcm_frames_f32(data, frames_count, final_data);
        } else if constexpr (std::is_same_v<T, int16_t>) {
          readed_frames = drflac_read_pcm_frames_s16(data, frames_count, final_data);
        }
      } else {
        // ошибка, наверное просто вернем 0
      }

      return readed_frames;
    }

    template <typename T>
    size_t get_frames_templ(
      drflac* data, std::vector<uint8_t> &buffer,
      const uint32_t al_buffer, const size_t frames_count,
      const uint16_t channels, const uint32_t sample_rate
    ) {
      static_assert(std::is_same_v<T, float> || std::is_same_v<T, int16_t>, "Supported formats is float32 and signed16");

      size_t readed_frames = 0;
      const size_t block_bytes_size = pcm_frames_to_bytes(frames_count, data->channels, data->bitsPerSample);
      if (buffer.size() < block_bytes_size) buffer.resize(block_bytes_size, 0);

      if (channels == 1 && data->channels != 1) {
        auto block_data = reinterpret_cast<T*>(buffer.data());
        if constexpr (std::is_same_v<T, float>) {
          readed_frames = drflac_read_pcm_frames_f32(data, frames_count, block_data); //.data()
        } else if constexpr (std::is_same_v<T, int16_t>) {
          readed_frames = drflac_read_pcm_frames_s16(data, frames_count, block_data); //.data()
        }

        make_mono(block_data, block_data, readed_frames, data->channels);

        const size_t buffer_size = pcm_frames_to_bytes(readed_frames, channels, data->bitsPerSample);
        al_call(alBufferData, al_buffer,
                to_al_format(channels, data->bitsPerSample),
                block_data, buffer_size, sample_rate);
      } else if (channels == data->channels) {
        auto block_data = reinterpret_cast<T*>(buffer.data());
        if constexpr (std::is_same_v<T, float>) {
          readed_frames = drflac_read_pcm_frames_f32(data, frames_count, block_data); //.data()
        } else if constexpr (std::is_same_v<T, int16_t>) {
          readed_frames = drflac_read_pcm_frames_s16(data, frames_count, block_data); //.data()
        }

        const size_t buffer_size = pcm_frames_to_bytes(readed_frames, channels, data->bitsPerSample);
        al_call(alBufferData, al_buffer,
                to_al_format(channels, data->bitsPerSample),
                block_data, buffer_size, sample_rate); //.data()
      } else {
        // ошибка, наверное просто вернем 0
      }

      return readed_frames;
    }

    // что то тут все не так, переписать
    // переписал, теперь вроде бы ошибок не должно быть, нужно научиться считать размер буфера по секундам
    size_t flac_decoder::get_frames(void* memory, const size_t frames_count, const uint16_t channels_override) {
      const uint16_t final_channels = channels_override != 0 ? channels_override : channels();

      size_t readed_frames = 0;

      if (bits_per_channel() <= 16) {
        readed_frames = get_frames_templ<int16_t>(data, buffer, memory, frames_count, final_channels);
      } else if (bits_per_channel() <= 32) {
        readed_frames = get_frames_templ<float>(data, buffer, memory, frames_count, final_channels);
      } else {
        utils::error{}("flac format with {} bits per channel is not supported", bits_per_channel());
      }

      return readed_frames;
    }

    size_t flac_decoder::get_frames(
      const uint32_t al_buffer,
      const size_t frames_count,
      const uint16_t channels_override,
      const uint32_t sample_rate_override
    ) {
      const uint16_t final_channels = channels_override != 0 ? channels_override : channels();
      const uint32_t final_sample_rate = sample_rate_override != 0 ? sample_rate_override : sample_rate();

      size_t readed_frames = 0;

      if (bits_per_channel() <= 16) {
        readed_frames = get_frames_templ<int16_t>(data, buffer, al_buffer, frames_count, final_channels, final_sample_rate);
      } else if (bits_per_channel() <= 32) {
        readed_frames = get_frames_templ<float>(data, buffer, al_buffer, frames_count, final_channels, final_sample_rate);
      } else {
        utils::error{}("flac format with {} bits per channel is not supported", bits_per_channel());
      }

      return readed_frames;
    }
  }
}
