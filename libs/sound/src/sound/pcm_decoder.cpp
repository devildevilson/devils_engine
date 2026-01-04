#include "pcm_decoder.h"

#include <cstring>

#include "AL/al.h"
#include "al_helper.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
  namespace sound {
    pcm_decoder::pcm_decoder(
      const void* memory,
      const size_t memory_size,
      const uint16_t channels,
      const uint32_t bits_pes_channel,
      const uint32_t sample_rate,
      const size_t frames_count
    ) : current_index(0), buffer(memory_size, 0) {
      memcpy(buffer.data(), memory, memory_size);
      m_channels = channels;
      m_bits_per_channel = bits_pes_channel;
      m_sample_rate = sample_rate;
      m_frames_count = frames_count;
    }

    pcm_decoder::pcm_decoder(decoder* parent) :
      current_index(0),
      buffer(pcm_frames_to_bytes(parent->frames_count(), parent->channels(), parent->bits_per_channel()), 0)
    {
      m_channels = parent->channels();
      m_bits_per_channel = parent->bits_per_channel();
      m_sample_rate = parent->sample_rate();
      m_frames_count = parent->frames_count();
      if (!parent->seek(0)) utils::error{}("Could not initialize pcm_decoder");
      parent->get_frames(buffer.data(), parent->frames_count());
    }

    pcm_decoder::~pcm_decoder() noexcept {}

    bool pcm_decoder::seek(const size_t seek_size) {
      current_index = pcm_frames_to_bytes(seek_size, channels(), bits_per_channel());
      current_index = std::min(current_index, buffer.size());
      return true;
    }

    size_t pcm_decoder::get_frames(void* memory, const size_t frames_count, const uint16_t channels_override) {
      const uint16_t final_channels = channels_override != 0 ? channels_override : channels();

      const size_t expected_bytes = pcm_frames_to_bytes(frames_count, channels(), bits_per_channel());
      const size_t bytes_size = std::min(expected_bytes, buffer.size() - current_index);
      if (bytes_size == 0) return 0;

      size_t readed_frames = 0;
      if (final_channels == 1 && channels() != 1) {
        // как понять тип данных оригинального файла? пока что я придерживаюсь того что возвращает to_al_format
        const int32_t format = to_al_format(channels(), bits_per_channel());
        readed_frames = bytes_to_pcm_frames(bytes_size, channels(), bits_per_channel());
        if (format == AL_FORMAT_STEREO8) {
          make_mono(reinterpret_cast<uint8_t*>(memory), &buffer[current_index], readed_frames, channels());
        } else if (format == AL_FORMAT_STEREO16) {
          make_mono(reinterpret_cast<int16_t*>(memory), reinterpret_cast<int16_t*>(&buffer[current_index]), readed_frames, channels());
        } else if (format == AL_FORMAT_STEREO_FLOAT32) {
          make_mono(reinterpret_cast<float*>(memory), reinterpret_cast<float*>(&buffer[current_index]), readed_frames, channels());
        }
        current_index += bytes_size;
      } else if (final_channels == channels()) {
        readed_frames = bytes_to_pcm_frames(bytes_size, final_channels, bits_per_channel());
        memcpy(memory, &buffer[current_index], bytes_size);
        current_index += bytes_size;
      } else {

      }

      return readed_frames;
    }

    size_t pcm_decoder::get_frames(
      const uint32_t al_buffer,
      const size_t frames_count,
      const uint16_t channels_override,
      const uint32_t sample_rate_override
    ) {
      const uint16_t final_channels = channels_override != 0 ? channels_override : channels();
      const uint32_t final_sample_rate = sample_rate_override != 0 ? sample_rate_override : sample_rate();

      const size_t expected_bytes = pcm_frames_to_bytes(frames_count, channels(), bits_per_channel());
      const size_t bytes_size = std::min(expected_bytes, buffer.size() - current_index);
      if (bytes_size == 0) return 0;

      size_t readed_frames = 0;
      if (final_channels == 1 && channels() != 1) {
        // как понять тип данных оригинального файла? пока что я придерживаюсь того что возвращает to_al_format
        const int32_t format = to_al_format(channels(), bits_per_channel());
        readed_frames = bytes_to_pcm_frames(bytes_size, channels(), bits_per_channel());
        const size_t expected_mono_bytes = pcm_frames_to_bytes(readed_frames, final_channels, bits_per_channel());
        std::vector<uint8_t> tmp_buffer(expected_mono_bytes, 0);
        if (format == AL_FORMAT_STEREO8) {
          make_mono(reinterpret_cast<uint8_t*>(tmp_buffer.data()), &buffer[current_index], readed_frames, channels());
        } else if (format == AL_FORMAT_STEREO16) {
          make_mono(reinterpret_cast<int16_t*>(tmp_buffer.data()), reinterpret_cast<int16_t*>(&buffer[current_index]), readed_frames, channels());
        } else if (format == AL_FORMAT_STEREO_FLOAT32) {
          make_mono(reinterpret_cast<float*>(tmp_buffer.data()), reinterpret_cast<float*>(&buffer[current_index]), readed_frames, channels());
        }
        al_call(alBufferData, al_buffer, format,
                tmp_buffer.data(), expected_mono_bytes, final_sample_rate);

        //if (buffer.size() - current_index < expected_bytes) throw std::runtime_error("check");
        current_index += bytes_size;
      } else if (final_channels == channels()) {
        readed_frames = bytes_to_pcm_frames(bytes_size, final_channels, bits_per_channel());
        const int32_t format = to_al_format(final_channels, bits_per_channel());
        const auto ptr = &buffer[current_index];

        al_call(alBufferData, al_buffer, format, ptr, bytes_size, final_sample_rate);

        current_index += bytes_size;
      } else {

      }

      //if (current_index >= buffer.size()) throw std::runtime_error("check2");
      return readed_frames;
    }
  }
}
