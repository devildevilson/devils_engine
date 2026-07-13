#include "devils_engine/catalogue/introspection.h"
#include "devils_engine/catalogue/logging.h" // logs().name() для префикса домена

namespace devils_engine {
namespace catalogue {
namespace {

std::string format_arguments(const std::span<const argument_view> args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i != 0) {
      out += ", ";
    }
    out += args[i].name;
    out += "=";
    out += !args[i].value.empty() ? args[i].value : "<opaque>";
  }
  return out;
}

} // namespace

namespace detail {

std::string_view shrink_placeholder(argument_value_buffer& buffer, const std::string_view type) {
  constexpr std::string_view prefix = "devils_engine::";
  constexpr std::string_view dots = "...";
  constexpr auto capacity = std::tuple_size_v<decltype(buffer.chars)>;

  const auto write_wrapped = [&](const std::string_view src) -> std::string_view {
    if (src.size() + 2 > capacity) {
      return {};
    }
    auto* out = buffer.chars.data();
    *out++ = '<';
    for (const auto c : src) {
      *out++ = c;
    }
    *out++ = '>';
    return std::string_view(buffer.chars.data(), size_t(out - buffer.chars.data()));
  };

  if (const auto full = write_wrapped(type); !full.empty()) {
    return full;
  }

  auto* out = buffer.chars.data();
  auto pos = size_t{0};
  while (pos < type.size()) {
    if (type.substr(pos, prefix.size()) == prefix) {
      pos += prefix.size();
      continue;
    }

    if (out == buffer.chars.data() + capacity) {
      break;
    }
    *out++ = type[pos++];
  }

  const auto without_engine_size = size_t(out - buffer.chars.data());
  if (without_engine_size + 2 <= capacity) {
    for (auto i = without_engine_size; i > 0; --i) {
      buffer.chars[i] = buffer.chars[i - 1];
    }
    buffer.chars[0] = '<';
    buffer.chars[without_engine_size + 1] = '>';
    return std::string_view(buffer.chars.data(), without_engine_size + 2);
  }

  constexpr auto payload_capacity = capacity - 2;
  constexpr auto prefix_capacity = payload_capacity - dots.size();
  const auto copy_count = std::min(without_engine_size, prefix_capacity);
  for (auto i = copy_count; i > 0; --i) {
    buffer.chars[i] = buffer.chars[i - 1];
  }
  out = buffer.chars.data();
  *out++ = '<';
  out += copy_count;
  for (const auto c : dots) {
    *out++ = c;
  }
  *out++ = '>';
  return std::string_view(buffer.chars.data(), size_t(out - buffer.chars.data()));
}

introspection_mode effective_mode(const introspection& in) noexcept {
  const auto base = static_cast<uint8_t>(in.mode);
  const auto floor = logs().enabled(in.log_domain, log_depth::trace)
                       ? static_cast<uint8_t>(introspection_mode::tracing)
                       : uint8_t{0};
  return static_cast<introspection_mode>(std::max(base, floor));
}

// enter: значим только для tracing/dump (вход + место вызова; dump — ещё аргументы).
void emit_enter(const introspection& in, const introspection_mode mode, const call_info& info) {
  switch (mode) {
    case introspection_mode::tracing:
      spdlog::info("[{}][trace] {}:{}: enter '{}'", logs().name(in.log_domain),
                   utils::make_sane_file_name(info.file), info.line, info.function_name);
      break;
    case introspection_mode::dump:
      spdlog::info("[{}][dump] {}:{}: enter '{}' ({})", logs().name(in.log_domain),
                   utils::make_sane_file_name(info.file), info.line, info.function_name,
                   format_arguments(info.arguments));
      break;
    default:
      break;
  }
}

// exit: невиртуальный switch по режиму — каждый берёт РОВНО что нужно (см. introspection_mode).
void emit_exit(const introspection& in, const introspection_mode mode, const call_info& info, const uint64_t elapsed_mcs) {
  switch (mode) {
    case introspection_mode::logging:
      spdlog::info("[{}][log] '{}' {} us", logs().name(in.log_domain), info.function_name, elapsed_mcs);
      break;
    case introspection_mode::statistics:
      if (in.stats != nullptr) {
        in.stats->record(info.function, info.function_name, info.file, info.line, elapsed_mcs);
      }
      break;
    case introspection_mode::tracing:
      spdlog::info("[{}][trace] {}:{}: exit '{}' ({} us)", logs().name(in.log_domain),
                   utils::make_sane_file_name(info.file), info.line, info.function_name, elapsed_mcs);
      break;
    case introspection_mode::dump:
      spdlog::info("[{}][dump] {}:{}: exit '{}' ({} us) ({})", logs().name(in.log_domain),
                   utils::make_sane_file_name(info.file), info.line, info.function_name, elapsed_mcs,
                   format_arguments(info.arguments));
      break;
    default:
      break;
  }
}

} // namespace detail

double statistics_store::function_record::recent_average_mcs() const noexcept {
  if (filled == 0) {
    return 0.0;
  }
  uint64_t sum = 0;
  for (size_t i = 0; i < filled; ++i) {
    sum += samples[i];
  }
  return double(sum) / double(filled);
}

void statistics_store::function_record::ordered_samples(std::vector<uint64_t>& out) const {
  out.clear();
  if (filled == 0) {
    return;
  }
  out.reserve(filled);
  // буфер не полон: [0, filled) уже по порядку; полон: самый старый в cursor, дальше по кругу.
  const size_t cap = samples.size();
  if (filled < cap) {
    for (size_t i = 0; i < filled; ++i) {
      out.push_back(samples[i]);
    }
  } else {
    for (size_t i = 0; i < cap; ++i) {
      out.push_back(samples[(cursor + i) % cap]);
    }
  }
}

statistics_store::statistics_store(const size_t window) noexcept : window_(window) {}

void statistics_store::record(const utils::id function, const std::string_view name,
                              const std::string_view file, const uint32_t line, const uint64_t elapsed_mcs) {
  ++total_calls_;

  auto [it, inserted] = records_.try_emplace(function);
  function_record& rec = it->second;
  if (inserted) {
    rec.function = function;
    rec.name = name;
    if (window_ != 0) {
      rec.samples.resize(window_, 0);
    }
  }
  rec.file = file; // место последнего вызова
  rec.line = line;

  ++rec.call_count;
  rec.total_mcs += elapsed_mcs;
  rec.min_mcs = std::min(rec.min_mcs, elapsed_mcs);
  rec.max_mcs = std::max(rec.max_mcs, elapsed_mcs);
  rec.last_mcs = elapsed_mcs;

  if (window_ != 0) {
    rec.samples[rec.cursor] = elapsed_mcs;
    rec.cursor = (rec.cursor + 1) % window_;
    if (rec.filled < window_) {
      ++rec.filled;
    }
  }
}

const statistics_store::function_record* statistics_store::find(const utils::id function) const noexcept {
  const auto it = records_.find(function);
  return it != records_.end() ? &it->second : nullptr;
}

double statistics_store::average_mcs(const utils::id function) const noexcept {
  const function_record* rec = find(function);
  return rec != nullptr ? rec->average_mcs() : 0.0;
}

void statistics_store::reset() noexcept {
  total_calls_ = 0;
  records_.clear();
}

} // namespace catalogue
} // namespace devils_engine
