#ifndef DEVILS_ENGINE_SOUND_AL_HELPER_H
#define DEVILS_ENGINE_SOUND_AL_HELPER_H

#include <type_traits>
#include <stdexcept>
#include <string_view>

#include "AL/al.h"
#include "AL/alc.h"
#include "AL/alext.h"
#include "spdlog/spdlog.h"
#include "devils_engine/utils/type_traits.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
  namespace sound {
    std::string_view get_al_error_str(ALenum err);
    std::string_view get_alc_error_str(ALenum err);
    std::string_view get_al_enum_error_str(ALenum err);
    std::string_view get_alc_enum_error_str(ALenum err);

    bool check_al_error();
    bool check_alc_error(ALCdevice* device);

    template <typename F, typename... Args>
    auto al_call_info(F al_func, Args&&... args) -> utils::function_result_type<F> {
      if constexpr (std::is_void_v<utils::function_result_type<F>>) {
        std::invoke(al_func, std::forward<Args>(args)...);
        check_al_error();
        return;
      } else {
        const auto ret = std::invoke(al_func, std::forward<Args>(args)...);
        check_al_error();
        return ret;
      }
    }

    template <typename F, typename... Args>
    auto al_call(F al_func, Args&&... args) -> utils::function_result_type<F> {
      if constexpr (std::is_void_v<utils::function_result_type<F>>) {
        std::invoke(al_func, std::forward<Args>(args)...);
        const bool no_error = check_al_error();
        if (!no_error) throw std::runtime_error("Got AL error");
        return;
      } else {
        const auto ret = std::invoke(al_func, std::forward<Args>(args)...);
        const bool no_error = check_al_error();
        if (!no_error) throw std::runtime_error("Got AL error");
        return ret;
      }
    }

    template <typename F>
    using first_arg_t = utils::function_argument_type<F, 0>;

    // template <typename F, typename... Args>
    // using alc_result_t = std::conditional_t<
    //   std::is_same<first_arg_t<F>, ALCdevice*>,
    //   std::invoke_result_t<F, ALCdevice*, Args...>,
    //   std::invoke_result_t<F, Args...>
    // >;

    template <typename F, typename... Args>
    auto alc_call_info(F alc_func, ALCdevice* device, Args&&... args) -> utils::function_result_type<F> {
      if constexpr (std::is_void_v<utils::function_result_type<F>>) {
        if constexpr (std::is_invocable_v<F, ALCdevice*, Args...>) {
          std::invoke(alc_func, device, std::forward<Args>(args)...);
        } else {
          std::invoke(alc_func, std::forward<Args>(args)...);
        }
        check_alc_error(device);
        return;
      } else {
        utils::function_result_type<F> ret;
        if constexpr (std::is_invocable_v<F, ALCdevice*, Args...>) {
          ret = std::invoke(alc_func, device, std::forward<Args>(args)...);
        } else {
          ret = std::invoke(alc_func, std::forward<Args>(args)...);
        }
        check_alc_error(device);
        return ret;
      }
    }

    template <typename F, typename... Args>
    auto alc_call(F alc_func, ALCdevice* device, Args&&... args) -> utils::function_result_type<F> {
      if constexpr (std::is_void_v<utils::function_result_type<F>>) {
        if constexpr (std::is_invocable_v<F, ALCdevice*, Args...>) {
          std::invoke(alc_func, device, std::forward<Args>(args)...);
        } else {
          std::invoke(alc_func, std::forward<Args>(args)...);
        }
        const bool no_error = check_alc_error(device);
        if (!no_error) throw std::runtime_error("Got ALC error");
        return;
      } else {
        utils::function_result_type<F> ret;
        if constexpr (std::is_invocable_v<F, ALCdevice*, Args...>) {
          ret = std::invoke(alc_func, device, std::forward<Args>(args)...);
        } else {
          ret = std::invoke(alc_func, std::forward<Args>(args)...);
        }
        const bool no_error = check_alc_error(device);
        if (!no_error) throw std::runtime_error("Got ALC error");
        return ret;
      }
    }

    void check_al_error(ALenum err);
    void check_alc_error(ALenum err);
    int32_t to_al_format(const uint16_t channels, const uint32_t bits_per_channel);
    uint32_t adjust_bits_per_channel(const uint32_t bits_per_channel);
    size_t pcm_frames_to_bytes(const size_t pcm_frames, const uint16_t channels, const uint32_t bits_per_sample);
    size_t bytes_to_pcm_frames(const size_t bytes, const uint16_t channels, const uint32_t bits_per_sample);
    size_t second_to_pcm_frames(const double seconds, const size_t sample_rate); // , const uint16_t channels
    //size_t second_to_pcm_frames_mono(const size_t seconds, const size_t sample_rate);
    double pcm_frames_to_seconds(const size_t pcm_frames, const size_t sample_rate);


    constexpr size_t align_to_lower_bound(const size_t size, const size_t alignment) {
      return size / alignment * alignment;
    }

    template <typename T>
    void make_mono(T* dest, const T* buffer, const size_t readed_frames, const uint16_t channels) {
      const size_t readed_block = readed_frames * channels;
      const size_t memory_size = readed_block / channels;
      //memset(dest, 0, memory_size * sizeof(T));
      for (size_t i = 0, j = 0; i < readed_block && j < memory_size; i += channels, j += 1) {
        for (size_t k = 0; k < channels; ++k) {
          dest[j] = k == 0 ? buffer[i+k] : dest[j] + buffer[i+k];
        }

        dest[j] /= float(channels);
      }
    }
  }
}

#endif
