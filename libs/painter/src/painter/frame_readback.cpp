#include "frame_readback.h"

#include <algorithm>
#include <cstring>

#include "vulkan_header.h"

#include "graphics_base.h"
#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace painter {

bool frame_readback::bind(const graphics_base& base, const names& n) {
  const bool was_busy = state_ == state::armed || state_ == state::submitted;
  const bool was_requested = state_ == state::requested;
  unbind();

  const uint32_t counter = base.find_counter(n.counter);
  const uint32_t readback = base.find_resource(n.readback);
  const uint32_t source = base.find_resource(n.source);
  if (counter == invalid_resource_slot || readback == invalid_resource_slot || source == invalid_resource_slot) {
    return false;
  }

  const auto& rb = DS_ASSERT_ARRAY_GET(base.resources, readback);
  if (role::is_image(rb.role) || !role::is_readback(rb.role)) {
    utils::error{}("frame readback: resource '{}' must be a buffer with role 'readback', it has role '{}'",
                   rb.name, role::to_string(rb.role));
  }
  if (rb.swap != counter) {
    utils::error{}("frame readback: resource '{}' must rotate on counter '{}' (swap = {}), it rotates on '{}'",
                   rb.name, n.counter, n.counter, DS_ASSERT_ARRAY_GET(base.counters, rb.swap).name);
  }
  const auto& src = DS_ASSERT_ARRAY_GET(base.resources, source);
  if (!role::is_image(src.role)) {
    utils::error{}("frame readback: source '{}' must be an image", src.name);
  }
  if (src.format_hint != uint32_t(VK_FORMAT_R8G8B8A8_UNORM) && src.format_hint != uint32_t(VK_FORMAT_B8G8R8A8_UNORM) &&
      src.format_hint != uint32_t(VK_FORMAT_R8G8B8A8_SRGB) && src.format_hint != uint32_t(VK_FORMAT_B8G8R8A8_SRGB)) {
    // Молча прочитать не то хуже отказа: байты ушли бы в файл как RGBA8.
    utils::error{}("frame readback: source '{}' has format {}, expected 8-bit RGBA/BGRA", src.name, src.format_hint);
  }

  // Пасс копирования — в АКТИВНОМ графе: сдвиг счётчика без пасса никогда не дал бы копии.
  uint32_t pass = invalid_resource_slot;
  if (base.current_render_graph_index < base.graphs.size()) {
    const auto& graph = DS_ASSERT_ARRAY_GET(base.graphs, base.current_render_graph_index);
    for (const auto index : graph.passes) {
      if (DS_ASSERT_ARRAY_GET(base.passes, index).counter == counter) {
        pass = index;
        break;
      }
    }
  }
  if (pass == invalid_resource_slot) {
    return false; // этот граф съёма не делает (например, меню)
  }

  counter_ = counter;
  readback_ = readback;
  source_ = source;
  pass_ = pass;
  // Запрос, ещё не ушедший в кадр, переживает смену графа. Ушедший — нет: его копия могла не
  // случиться, а буфер мог пересоздаться.
  state_ = was_busy ? state::lost : was_requested ? state::requested : state::idle;
  return true;
}

void frame_readback::unbind() noexcept {
  source_ = readback_ = counter_ = pass_ = invalid;
  if (state_ == state::armed || state_ == state::submitted) {
    state_ = state::lost;
  }
  frames_waited_ = 0;
}

bool frame_readback::request() noexcept {
  if (state_ != state::idle) {
    return false;
  }
  // Без привязки запрос ждёт её: граф появляется позже окна, а съём просят с первых кадров.
  state_ = state::requested;
  return true;
}

void frame_readback::before_frame(graphics_base& base) {
  if (!bound() || state_ != state::requested) {
    return;
  }
  base.inc_counter(counter_);
  state_ = state::armed;
}

void frame_readback::after_submit(const graphics_base& base) {
  if (!bound() || state_ != state::armed) {
    return;
  }
  // Значение счётчика фиксируется пассом только после успешной отправки. Кадр, не дошедший до неё,
  // оставляет съём «вооружённым», и пасс исполнится в следующем.
  const uint32_t value = DS_ASSERT_ARRAY_GET(base.counters, counter_).get_value();
  if (pass_ >= base.pass_observed_counter.size() || base.pass_observed_counter[pass_] != value) {
    return;
  }
  const auto [bytes, ext] = DS_ASSERT_ARRAY_GET(base.resources, source_).compute_frame_size(&base);
  (void)bytes;
  width_ = ext.x;
  height_ = ext.y;
  frames_waited_ = 0;
  state_ = state::submitted;
}

frame_readback::status frame_readback::collect(graphics_base& base, captured_frame& out) {
  if (state_ == state::lost) {
    state_ = state::idle;
    return status::lost;
  }
  if (state_ == state::idle) {
    return status::idle;
  }
  if (state_ != state::submitted || !bound()) {
    return status::pending;
  }
  // collect зовут после каждого prepare_frame. За frames_in_flight подготовок очередь доходит до
  // слота кадра-писателя, и prepare_frame ждёт ИМЕННО его fence: копия завершена.
  frames_waited_ += 1;
  if (frames_waited_ < base.frames_in_flight()) {
    return status::pending;
  }

  const auto [bytes, ext] = DS_ASSERT_ARRAY_GET(base.resources, source_).compute_frame_size(&base);
  (void)bytes;
  if (ext.x != width_ || ext.y != height_) {
    state_ = state::idle;
    return status::lost; // буфер пересоздан под новый размер окна
  }

  const size_t size = size_t(width_) * height_ * 4;
  const auto frame = base.get_current_buffer_resource_frame(readback_);
  if (frame.mapped == nullptr || frame.sub.size < size) {
    utils::error{}("frame readback: buffer '{}' holds {} bytes per copy, a {}x{} frame needs {}",
                   frame.name, frame.sub.size, width_, height_, size);
  }
  base.invalidate_buffer_resource_frame(readback_);

  const auto& src = DS_ASSERT_ARRAY_GET(base.resources, source_);
  out.width = width_;
  out.height = height_;
  out.bgra = src.format_hint == uint32_t(VK_FORMAT_B8G8R8A8_UNORM) || src.format_hint == uint32_t(VK_FORMAT_B8G8R8A8_SRGB);
  out.pixels.resize(size);
  std::memcpy(out.pixels.data(), static_cast<const uint8_t*>(frame.mapped) + frame.sub.offset, size);
  state_ = state::idle;
  return status::ready;
}

} // namespace painter
} // namespace devils_engine
