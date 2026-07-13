#include "decoder.h"

namespace devils_engine {
namespace sound {
decoder::decoder() noexcept
  : m_format(format::unknown), m_channels(0), m_sample_rate(0), m_frames_count(0) {}

enum format decoder::format() const noexcept {
  return m_format;
}

uint16_t decoder::channels() const noexcept {
  return m_channels;
}

uint32_t decoder::sample_rate() const noexcept {
  return m_sample_rate;
}

size_t decoder::frames_count() const noexcept {
  return m_frames_count;
}
} // namespace sound
} // namespace devils_engine
