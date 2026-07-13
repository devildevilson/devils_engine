#include <algorithm>
#include <limits>

#include <opusfile.h>

#include "AL/al.h"
#include "al_helper.h"
#include "devils_engine/utils/core.h"
#include "opus_decoder.h"

namespace devils_engine {
namespace sound {
namespace {
constexpr uint32_t opus_output_sample_rate = 48000;

int checked_float_count(const size_t frames, const uint16_t channels) {
  const size_t count = frames * size_t(channels);
  if (count > size_t(std::numeric_limits<int>::max())) {
    utils::error{}("opus_decoder: requested frame block is too large");
  }

  return static_cast<int>(count);
}
} // namespace

opus_decoder::opus_decoder(const std::string_view& name, const void* memory, const size_t memory_size) : data(nullptr) {
  int error = 0;
  data = op_open_memory(reinterpret_cast<const unsigned char*>(memory), memory_size, &error);
  if (data == nullptr) {
    utils::error{}("Failed to parse opus resource '{}' (error {})", name, error);
  }

  const int channels_count = op_channel_count(data, -1);
  const ogg_int64_t total_frames = op_pcm_total(data, -1);
  if (channels_count <= 0 || total_frames < 0) {
    utils::error{}("Failed to read opus resource '{}' metadata", name);
  }

  m_sample_rate = opus_output_sample_rate;
  m_format = format::f32;
  m_channels = static_cast<uint16_t>(channels_count);
  m_frames_count = static_cast<size_t>(total_frames);
}

opus_decoder::~opus_decoder() noexcept {
  op_free(data);
}

bool opus_decoder::seek(const size_t seek_size) {
  return op_pcm_seek(data, static_cast<ogg_int64_t>(seek_size)) == 0;
}

size_t opus_decoder::get_frames(void* memory, const size_t frames_count, const uint16_t channels_override) {
  const uint16_t final_channels = channels_override == 2 ? uint16_t(2) : channels();
  const int sample_capacity = checked_float_count(frames_count, final_channels);
  int link = -1;
  const int read_frames = final_channels == 2 && channels() != 2
                            ? op_read_float_stereo(data, reinterpret_cast<float*>(memory), sample_capacity)
                            : op_read_float(data, reinterpret_cast<float*>(memory), sample_capacity, &link);
  if (read_frames < 0) {
    utils::warn("opus_decoder: decode error {}", read_frames);
    return 0;
  }

  return static_cast<size_t>(read_frames);
}

size_t opus_decoder::get_frames(
  const uint32_t al_buffer,
  const size_t frames_count,
  const uint16_t channels_override,
  const uint32_t sample_rate_override) {
  const uint16_t final_channels = channels_override == 2 ? uint16_t(2) : channels();
  const uint32_t final_sample_rate = sample_rate_override != 0 ? sample_rate_override : sample_rate();
  const size_t samples_count = frames_count * size_t(final_channels);
  if (buffer.size() < samples_count) {
    buffer.resize(samples_count, 0.0f);
  }

  const size_t read_frames = get_frames(buffer.data(), frames_count, final_channels);
  const size_t read_bytes = read_frames * size_t(final_channels) * sizeof(float);
  al_call(alBufferData, al_buffer,
          to_al_format(final_channels, 32),
          buffer.data(), read_bytes,
          final_sample_rate);

  return read_frames;
}
} // namespace sound
} // namespace devils_engine
