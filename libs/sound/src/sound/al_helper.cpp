#include "al_helper.h"

namespace devils_engine {
  namespace sound {
    std::string_view get_al_error_str(ALenum err) {
      switch(err) {
        case AL_INVALID_NAME: return "a bad name (ID) was passed to an OpenAL function";
        case AL_INVALID_ENUM: return "an invalid enum value was passed to an OpenAL function";
        case AL_INVALID_VALUE: return "an invalid value was passed to an OpenAL function";
        case AL_INVALID_OPERATION: return "the requested operation is not valid";
        case AL_OUT_OF_MEMORY: return "the requested operation resulted in OpenAL running out of memory";
      }

      return "UNKNOWN AL ERROR";
    }

    std::string_view get_alc_error_str(ALenum err) {
      switch(err) {
        case ALC_INVALID_DEVICE: return "Invalid device handle";
        case ALC_INVALID_CONTEXT: return "Invalid context handle";
        case ALC_INVALID_ENUM: return "Invalid enumeration passed to an ALC call";
        case ALC_INVALID_VALUE: return "Invalid value passed to an ALC call";
        case ALC_OUT_OF_MEMORY: return "Out of memory";
      }

      return "UNKNOWN AL ERROR";
    }

    std::string_view get_al_enum_error_str(ALenum err) {
      switch(err) {
        case AL_INVALID_NAME: return "AL_INVALID_NAME";
        case AL_INVALID_ENUM: return "AL_INVALID_ENUM";
        case AL_INVALID_VALUE: return "AL_INVALID_VALUE";
        case AL_INVALID_OPERATION: return "AL_INVALID_OPERATION";
        case AL_OUT_OF_MEMORY: return "AL_OUT_OF_MEMORY";
      }

      return "UNKNOWN AL ERROR";
    }

    std::string_view get_alc_enum_error_str(ALenum err) {
      switch(err) {
        case ALC_INVALID_DEVICE: return "ALC_INVALID_DEVICE";
        case ALC_INVALID_CONTEXT: return "ALC_INVALID_CONTEXT";
        case ALC_INVALID_ENUM: return "ALC_INVALID_ENUM";
        case ALC_INVALID_VALUE: return "ALC_INVALID_VALUE";
        case ALC_OUT_OF_MEMORY: return "ALC_OUT_OF_MEMORY";
      }

      return "UNKNOWN ALC ERROR";
    }

    bool check_al_error() {
      const auto error = alGetError();
      if (error == AL_NO_ERROR) return true;

      const auto error_str = get_al_error_str(error);
      //const auto error_str = alGetString(error); // не дает информацию
      const auto enum_str = get_al_enum_error_str(error);
      spdlog::error("Got AL error {} ({}): {}", enum_str, error, error_str);

      return false;
    }

    bool check_alc_error(ALCdevice* device) {
      const auto error = alcGetError(device);
      if (error == ALC_NO_ERROR) return true;

      const auto error_str = get_alc_error_str(error);
      //const auto error_str = alcGetString(device, error);
      const auto enum_str = get_alc_enum_error_str(error);
      spdlog::error("Got ALC error {} ({}): {}", enum_str, error, error_str);

      return false;
    }

    void check_al_error(ALenum err) {
      if (err == AL_NO_ERROR) return;

      const auto error_str = get_al_error_str(err);
      const auto enum_str = get_al_enum_error_str(err);
      utils::error("Got AL error {} ({}): {}", enum_str, err, error_str);
    }

    void check_alc_error(ALenum err) {
      if (err == ALC_NO_ERROR) return;

      const auto error_str = get_alc_error_str(err);
      const auto enum_str = get_alc_enum_error_str(err);
      utils::error("Got ALC error {} ({}): {}", enum_str, err, error_str);
    }

    int32_t to_al_format(const uint16_t channels, const uint32_t bitsPerChannel) {
      const bool stereo = channels > 1;

      if (bitsPerChannel <= 8) {
        if (stereo) return AL_FORMAT_STEREO8;
        else return AL_FORMAT_MONO8;
      } else if (bitsPerChannel <= 16) {
        if (stereo) return AL_FORMAT_STEREO16;
        else return AL_FORMAT_MONO16;
      } else if (bitsPerChannel <= 32) {
        if (stereo) return AL_FORMAT_STEREO_FLOAT32;
        else return AL_FORMAT_MONO_FLOAT32;
      }

      // switch (bitsPerChannel) {
      //   case 32:
      //     if (stereo) return AL_FORMAT_STEREO_FLOAT32;
      //     else return AL_FORMAT_MONO_FLOAT32;
      //   case 16:
      //     if (stereo) return AL_FORMAT_STEREO16;
      //     else return AL_FORMAT_MONO16;
      //   case 8:
      //     if (stereo) return AL_FORMAT_STEREO8;
      //     else return AL_FORMAT_MONO8;
      // }

      return -1;
    }

    uint32_t adjust_bits_per_channel(const uint32_t bits_per_channel) {
      if (bits_per_channel >= 32) return bits_per_channel;
      return utils::next_power_of_2(bits_per_channel);
    }

    size_t pcm_frames_to_bytes(const size_t pcm_frames, const uint16_t channels, const uint32_t bits_per_sample) {
      return pcm_frames * channels * (bits_per_sample/8);
    }

    size_t bytes_to_pcm_frames(const size_t bytes, const uint16_t channels, const uint32_t bits_per_sample) {
      return (bytes / channels) / (bits_per_sample/8);
    }

    size_t second_to_pcm_frames(const double seconds, const size_t sample_rate) { // , const uint16_t channels
      return seconds * sample_rate;
    }

    // size_t second_to_pcm_frames_mono(const size_t seconds, const size_t sample_rate) {
    //   // я умножаю на количество каналов внутри декодера
    //   // чтобы если что сделать конвертацию в моно звук
    //   return seconds * sample_rate; // * channels
    // }

    double pcm_frames_to_seconds(const size_t pcm_frames, const size_t sample_rate) {
      return double(pcm_frames) / double(sample_rate);
    }
  }
}
