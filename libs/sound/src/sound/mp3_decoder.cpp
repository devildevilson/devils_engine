#include "mp3_decoder.h"

#define DR_MP3_NO_STDIO
#define DR_MP3_IMPLEMENTATION
#include "dr_mp3.h"

#include "AL/al.h"
#include "al_helper.h"

#include <fstream>
#include <vector>

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
    mp3_decoder::mp3_decoder(const std::string_view &name, const void* memory, const size_t memory_size) {
      const drmp3_allocation_callbacks allocation_callbacks {
        nullptr,
        my_malloc,
        my_realloc,
        my_free
      };

      if (!drmp3_init_memory(&data, memory, memory_size, &allocation_callbacks)) {
        utils::error("Failed to parse mp3 resource '{}'", name);
      }

      m_sample_rate = data.sampleRate;
      m_bits_per_channel = 32;
      m_frames_count = drmp3_get_pcm_frame_count(&data);
      m_channels = data.channels;
    }

    mp3_decoder::~mp3_decoder() noexcept {
      drmp3_uninit(&data);
    }

    bool mp3_decoder::seek(const size_t seek_size) {
      return drmp3_seek_to_pcm_frame(&data, seek_size);
    }

    // что то тут все не так, переписать
    // переписал, теперь вроде бы ошибок не должно быть, нужно научиться считать размер буфера по секундам
    size_t mp3_decoder::get_frames(void* memory, const size_t frames_count, const uint16_t channels_override) {
      const uint16_t final_channels = channels_override != 0 ? channels_override : channels();

      auto* final_data = reinterpret_cast<float*>(memory);
      size_t readed_frames = 0;
      if (final_channels == 1 && data.channels != 1) {
        const size_t block_bytes_size = pcm_frames_to_bytes(frames_count, data.channels, bits_per_channel());
        if (buffer.size() < block_bytes_size) buffer.resize(block_bytes_size, 0);
        auto block_data = reinterpret_cast<float*>(buffer.data());

        readed_frames = drmp3_read_pcm_frames_f32(&data, frames_count, block_data);

        make_mono(final_data, block_data, readed_frames, data.channels);
      } else if (final_channels == data.channels) {
        readed_frames = drmp3_read_pcm_frames_f32(&data, frames_count, final_data);
      } else {
        // ошибка, наверное просто вернем 0
      }

      return readed_frames;
    }

    size_t mp3_decoder::get_frames(
      const uint32_t al_buffer,
      const size_t frames_count,
      const uint16_t channels_override,
      const uint32_t sample_rate_override
    ) {
      const uint16_t final_channels = channels_override != 0 ? channels_override : channels();
      const uint32_t final_sample_rate = sample_rate_override != 0 ? sample_rate_override : sample_rate();

      size_t readed_frames = 0;
      const size_t block_bytes_size = pcm_frames_to_bytes(frames_count, data.channels, bits_per_channel());
      if (buffer.size() < block_bytes_size) buffer.resize(block_bytes_size, 0);

      if (final_channels == 1 && data.channels != 1) {
        auto block_data = reinterpret_cast<float*>(buffer.data());
        readed_frames = drmp3_read_pcm_frames_f32(&data, frames_count, block_data);

        make_mono(block_data, block_data, readed_frames, data.channels);
        const size_t buffer_size = pcm_frames_to_bytes(readed_frames, final_channels, bits_per_channel());
        al_call(alBufferData, al_buffer,
                to_al_format(final_channels, bits_per_channel()),
                block_data, buffer_size, final_sample_rate); //.data()
      } else if (final_channels == data.channels) {
        auto block_data = reinterpret_cast<float*>(buffer.data());
        readed_frames = drmp3_read_pcm_frames_f32(&data, frames_count, block_data); //.data()
        const size_t buffer_size = pcm_frames_to_bytes(readed_frames, final_channels, bits_per_channel());
        al_call(alBufferData, al_buffer,
                to_al_format(final_channels, bits_per_channel()),
                block_data, buffer_size, final_sample_rate); //.data()

      } else {
        // ошибка, наверное просто вернем 0
      }

      return readed_frames;
    }
  }
}
