#ifndef DEVILS_ENGINE_CATALOGUE_LOGGING_H
#define DEVILS_ENGINE_CATALOGUE_LOGGING_H

#include <array>
#include <atomic>
#include <cstdint>
#include <format>
#include <source_location>
#include <string_view>

#include "devils_engine/utils/core.h" // spdlog + utils::info + make_sane_file_name (сжатие пути)

namespace devils_engine {
namespace catalogue {

// Глубина логгирования ВНУТРИ домена. Ортогональна spdlog severity (warn/error всегда включены —
// это про важность). Домен включают на нужную глубину в рантайме (конфиг/UI), даже в release.
//   off   — молчим;
//   info  — редкие сообщения домена (не путать с базовым always-on utils::info);
//   flow  — важные переходы + периодические срезы («что происходит»);
//   trace — полная трассировка пайплайна (плотно).
enum class log_depth : uint8_t { off = 0, info = 1, flow = 2, trace = 3 };

// Открытый набор доменов: id = индекс в фикс. таблице уровней. Домены — НЕ enum, а constexpr
// константы: новый слой логгирования = новая константа (+ регистрация имени в register_*_domains).
// Движковые домены занимают [0, engine_count); проектные продолжают нумерацию дальше.
namespace log_domain {
  inline constexpr uint32_t main         = 0;
  inline constexpr uint32_t assets       = 1;
  inline constexpr uint32_t sound        = 2;
  inline constexpr uint32_t render       = 3;
  inline constexpr uint32_t ui           = 4;
  inline constexpr uint32_t gameplay     = 5;
  inline constexpr uint32_t resource     = 6;
  inline constexpr uint32_t demiurg      = 7;
  inline constexpr uint32_t engine_count = 8;
}

// Рантайм-реестр уровней по домену. Атомики — уровень меняют из конфига/UI/любого потока;
// hot-path чтение (гейт) — relaxed (логам строгий порядок не нужен).
class log_registry {
public:
  static constexpr uint32_t capacity = 64; // запас под проектные домены

  void register_domain(uint32_t id, std::string_view name) noexcept;
  void set_level(uint32_t id, log_depth d) noexcept;
  bool set_level(std::string_view name, log_depth d) noexcept; // конфиг/UI: по имени → false если нет
  log_depth level(uint32_t id) const noexcept;
  std::string_view name(uint32_t id) const noexcept;

  // hot-path гейт: relaxed-load + сравнение. near-zero cost когда домен off.
  bool enabled(const uint32_t id, const log_depth d) const noexcept {
    return id < capacity && uint8_t(d) <= levels_[id].load(std::memory_order_relaxed);
  }

private:
  std::array<std::atomic<uint8_t>, capacity> levels_{}; // 0 = off по умолчанию
  std::array<std::string_view, capacity> names_{};
};

log_registry& logs() noexcept;                     // singleton
void register_engine_domains() noexcept;           // имена базового набора
bool parse_log_depth(std::string_view s, log_depth& out) noexcept; // "off/info/flow/trace"
std::string_view log_depth_name(log_depth d) noexcept;

// Настройка spdlog: консоль (опц.) + ротационный файл (опц.). Зовётся один раз на старте.
// Наш доменный гейт контролирует ГЛУБИНУ; spdlog оставляем на info, severity (warn/error) — его.
void init_logging(std::string_view file_path, bool console = true);

// Emit — вызывается ТОЛЬКО когда гейт уже пропустил (из макроса). Префикс [domain][depth].
template <typename... Args>
void log_line(const uint32_t domain, const log_depth depth, const std::format_string<Args...> fmt, Args&&... args) {
  spdlog::info("[{}][{}] {}", logs().name(domain), log_depth_name(depth),
               std::format(fmt, std::forward<Args>(args)...));
}

// trace-emit с МЕСТОМ вызова (file:line, путь сжат до 2 сегментов как в utils::error{}).
// Отдельно от log_line — file:line полезен только на trace, а source_location не бесплатен.
template <typename... Args>
void trace_line(const uint32_t domain, const std::source_location loc,
                const std::format_string<Args...> fmt, Args&&... args) {
  spdlog::info("[{}][trace] {}:{}: {}", logs().name(domain),
               utils::make_sane_file_name(loc.file_name()), loc.line(),
               std::format(fmt, std::forward<Args>(args)...));
}

}
}

// Гейт-then-emit: аргументы форматируются ТОЛЬКО если домен включён на нужную глубину →
// near-zero cost когда выключено (в release тоже). DEPTH — имя уровня: info / flow / trace.
//   DE_LOG(catalogue::log_domain::sound, flow, "play {}", id);
#define DE_LOG(DOMAIN, DEPTH, ...)                                                              \
  do {                                                                                          \
    if (::devils_engine::catalogue::logs().enabled((DOMAIN), ::devils_engine::catalogue::log_depth::DEPTH)) \
      ::devils_engine::catalogue::log_line((DOMAIN), ::devils_engine::catalogue::log_depth::DEPTH, __VA_ARGS__); \
  } while (0)

// trace-тир: полная трассировка пайплайна. Фиксирует место вызова (file:line, путь сжат).
// Гейт на trace-глубине домена → в info/flow/off не стоит ничего (аргументы не форматируются).
//   DE_TRACE(catalogue::log_domain::gameplay, "cognition batch: {} due", n);
#define DE_TRACE(DOMAIN, ...)                                                                    \
  do {                                                                                           \
    if (::devils_engine::catalogue::logs().enabled((DOMAIN), ::devils_engine::catalogue::log_depth::trace)) \
      ::devils_engine::catalogue::trace_line((DOMAIN), ::std::source_location::current(), __VA_ARGS__); \
  } while (0)

#endif
