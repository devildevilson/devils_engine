#include "core/launch_options.h"

#include <charconv>
#include <cstdint>
#include <string_view>

#include <devils_engine/network/endpoint.h>
#include <devils_engine/painter/image_file.h>

namespace frontier_online {
namespace core {

namespace {

template <typename T>
bool parse_number(const std::string& text, T& value) {
  const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
  return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}

} // namespace

bool parse_launch_options(const devils_engine::simul::launch_arguments& arguments, launch_options& out,
                          std::string& problem) {
  bool frames_given = false;
  bool input_lead_given = false;
  for (const auto& arg : arguments.all()) {
    if (arg.name == "dump" && arg.has_value) { // снять кадр в этот файл (.avif, .png, .ppm)
      const auto format = devils_engine::painter::image_file_format_from_path(arg.value);
      if (!format) {
        problem = "--dump must name an .avif, .png or .ppm file";
        return false;
      }
      if (!devils_engine::painter::image_file_format_available(*format)) {
        problem = "this build has no AVIF encoder; use a .png --dump path";
        return false;
      }
      out.capture.path = arg.value;
      if (out.capture.at_frame == 0) out.capture.at_frame = 1;
      continue;
    }
    if (arg.name == "frames" && arg.has_value) { // на каком кадре снимать
      frames_given = true;
      uint64_t value = 0;
      if (!parse_number(arg.value, value) || value == 0) {
        problem = "bad --frames value";
        return false;
      }
      out.capture.at_frame = value;
      continue;
    }
    if (arg.name == "stay" && !arg.has_value) { // снять кадр и продолжать жить (смотреть глазами)
      frames_given = true;
      out.capture.exit_after = false;
      continue;
    }
    if (arg.name == "join" && arg.has_value) { // живая сессия: HOST:PORT авторитета
      if (!network_client_available()) {
        problem = "this build has no network backend";
        return false;
      }
      if (!devils_engine::network::parse_endpoint(arg.value)) {
        problem = "--join must be IPV4:PORT";
        return false;
      }
      out.join.endpoint = arg.value;
      continue;
    }
    if (arg.name == "input-lead" && arg.has_value) { // на сколько тиков вперёд назначать свой клик
      input_lead_given = true;
      uint32_t value = 0;
      if (!parse_number(arg.value, value) || value == 0 || value > 32) {
        problem = "--input-lead must be 1..32 ticks";
        return false;
      }
      out.join.input_lead_ticks = value;
      continue;
    }
    problem = "unknown argument '" + arg.text + "'";
    return false;
  }
  // Опция без своего режима — тоже отказ: `--frames=120` без `--dump` открыл бы окно навсегда.
  if (frames_given && out.capture.path.empty()) {
    problem = "--frames and --stay need --dump";
    return false;
  }
  if (input_lead_given && out.join.endpoint.empty()) {
    problem = "--input-lead needs --join";
    return false;
  }
  return true;
}

} // namespace core
} // namespace frontier_online
