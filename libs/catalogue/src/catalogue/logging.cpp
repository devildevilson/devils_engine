#include <memory>
#include <vector>

#include <spdlog/sinks/rotating_file_sink.h>
#include <spdlog/sinks/stdout_color_sinks.h>

#include "devils_engine/catalogue/logging.h"

namespace devils_engine {
namespace catalogue {

log_registry& logs() noexcept {
  static log_registry registry;
  return registry;
}

void log_registry::register_domain(const uint32_t id, const std::string_view name) noexcept {
  if (id < capacity) {
    names_[id] = name;
  }
}

void log_registry::set_level(const uint32_t id, const log_depth d) noexcept {
  if (id < capacity) {
    levels_[id].store(uint8_t(d), std::memory_order_release);
  }
}

bool log_registry::set_level(const std::string_view name, const log_depth d) noexcept {
  for (uint32_t i = 0; i < capacity; ++i) {
    if (!names_[i].empty() && names_[i] == name) {
      set_level(i, d);
      return true;
    }
  }
  return false;
}

log_depth log_registry::level(const uint32_t id) const noexcept {
  return id < capacity ? log_depth(levels_[id].load(std::memory_order_acquire)) : log_depth::off;
}

std::string_view log_registry::name(const uint32_t id) const noexcept {
  if (id >= capacity || names_[id].empty()) {
    return "?";
  }
  return names_[id];
}

void register_engine_domains() noexcept {
  auto& r = logs();
  r.register_domain(log_domain::main, "main");
  r.register_domain(log_domain::assets, "assets");
  r.register_domain(log_domain::sound, "sound");
  r.register_domain(log_domain::render, "render");
  r.register_domain(log_domain::ui, "ui");
  r.register_domain(log_domain::gameplay, "gameplay");
  r.register_domain(log_domain::resource, "resource");
  r.register_domain(log_domain::demiurg, "demiurg");
}

bool parse_log_depth(const std::string_view s, log_depth& out) noexcept {
  if (s == "off") {
    out = log_depth::off;
  } else if (s == "info") {
    out = log_depth::info;
  } else if (s == "flow") {
    out = log_depth::flow;
  } else if (s == "trace") {
    out = log_depth::trace;
  } else {
    return false;
  }
  return true;
}

std::string_view log_depth_name(const log_depth d) noexcept {
  switch (d) {
    case log_depth::off: return "off";
    case log_depth::info: return "info";
    case log_depth::flow: return "flow";
    case log_depth::trace: return "trace";
  }
  return "?";
}

void init_logging(const std::string_view file_path, const bool console) {
  std::vector<spdlog::sink_ptr> sinks;
  if (console) {
    sinks.push_back(std::make_shared<spdlog::sinks::stdout_color_sink_mt>());
  }
  if (!file_path.empty()) {
    // ротация: до 5 файлов по 8 МБ — не разрастётся бесконтрольно
    sinks.push_back(std::make_shared<spdlog::sinks::rotating_file_sink_mt>(
      std::string(file_path), size_t(8) * 1024 * 1024, 5));
  }
  if (sinks.empty()) {
    return; // ничего не просили — оставляем дефолтный логгер spdlog
  }

  auto logger = std::make_shared<spdlog::logger>("devils", sinks.begin(), sinks.end());
  logger->set_level(spdlog::level::info); // глубину контролирует НАШ доменный гейт, не spdlog
  logger->flush_on(spdlog::level::warn);
  spdlog::set_default_logger(logger);
}

} // namespace catalogue
} // namespace devils_engine
