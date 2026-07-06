#include "devils_engine/catalogue/introspection.h"

#include <sstream>

namespace devils_engine {
namespace catalogue {
namespace {

static std::string format_arguments(const std::span<const argument_view> args) {
  std::string out;
  for (size_t i = 0; i < args.size(); ++i) {
    if (i != 0) out += ", ";
    out += args[i].name;
    out += "=";
    out += !args[i].value.empty() ? args[i].value : "<opaque>";
  }
  return out;
}

}

// Трассировка функций: вход/выход + место вызова (file:line, путь сжат до 2 сегментов как
// в utils::error{}) + perf на выходе. file/line берутся из call_info (их пишет loc_fn_t по
// std::source_location места вызова). Совпадает по формату с DE_TRACE.
call_decision trace_introspection::enter(const call_info& info) {
  utils::info("[trace] {}:{}: enter '{}'", utils::make_sane_file_name(info.file), info.line, info.function_name);
  return call_decision::execute;
}

void trace_introspection::exit(const call_info& info, const uint64_t elapsed_mcs) {
  utils::info("[trace] {}:{}: exit '{}' ({} us)", utils::make_sane_file_name(info.file), info.line, info.function_name, elapsed_mcs);
}

void trace_introspection::skipped(const call_info& info) {
  utils::info("[trace] {}:{}: skipped '{}'", utils::make_sane_file_name(info.file), info.line, info.function_name);
}

call_decision timing_introspection::enter(const call_info&) {
  return call_decision::execute;
}

void timing_introspection::exit(const call_info& info, const uint64_t elapsed_mcs) {
  const std::string args = format_arguments(info.arguments);
  utils::info("'{}' took {} mcs ({})", info.function_name, elapsed_mcs, args);
}

void timing_introspection::skipped(const call_info& info) {
  utils::info("'{}' skipped", info.function_name);
}

call_decision dry_run_introspection::enter(const call_info& info) {
  utils::info("dry-run '{}'", info.function_name);
  return call_decision::skip;
}

void dry_run_introspection::exit(const call_info&, uint64_t) {}

void dry_run_introspection::skipped(const call_info& info) {
  const std::string args = format_arguments(info.arguments);
  utils::info("dry-run skipped '{}' ({})", info.function_name, args);
}

double statistics_introspection::function_record::recent_average_mcs() const noexcept {
  if (filled == 0) return 0.0;
  uint64_t sum = 0;
  for (size_t i = 0; i < filled; ++i) sum += samples[i];
  return double(sum) / double(filled);
}

void statistics_introspection::function_record::ordered_samples(std::vector<uint64_t>& out) const {
  out.clear();
  if (filled == 0) return;
  out.reserve(filled);
  // буфер ещё не полон: замеры лежат в [0, filled) уже по порядку;
  // буфер полон (filled == размера): самый старый — в cursor, дальше по кругу.
  const size_t cap = samples.size();
  if (filled < cap) {
    for (size_t i = 0; i < filled; ++i) out.push_back(samples[i]);
  } else {
    for (size_t i = 0; i < cap; ++i) out.push_back(samples[(cursor + i) % cap]);
  }
}

statistics_introspection::statistics_introspection(const size_t window) noexcept : window_(window) {}

call_decision statistics_introspection::enter(const call_info&) {
  return call_decision::execute;
}

void statistics_introspection::exit(const call_info& info, const uint64_t elapsed_mcs) {
  ++total_calls_;

  auto [it, inserted] = records_.try_emplace(info.function);
  function_record& rec = it->second;
  if (inserted) {
    rec.function = info.function;
    rec.name = info.function_name;
    rec.file = info.file;
    rec.line = info.line;
    if (window_ != 0) rec.samples.resize(window_, 0);
  }

  ++rec.call_count;
  rec.total_mcs += elapsed_mcs;
  rec.min_mcs = std::min(rec.min_mcs, elapsed_mcs);
  rec.max_mcs = std::max(rec.max_mcs, elapsed_mcs);
  rec.last_mcs = elapsed_mcs;

  if (window_ != 0) {
    rec.samples[rec.cursor] = elapsed_mcs;
    rec.cursor = (rec.cursor + 1) % window_;
    if (rec.filled < window_) ++rec.filled;
  }
}

void statistics_introspection::skipped(const call_info&) {}

const statistics_introspection::function_record* statistics_introspection::find(const utils::id function) const noexcept {
  const auto it = records_.find(function);
  return it != records_.end() ? &it->second : nullptr;
}

double statistics_introspection::average_mcs(const utils::id function) const noexcept {
  const function_record* rec = find(function);
  return rec != nullptr ? rec->average_mcs() : 0.0;
}

void statistics_introspection::reset() noexcept {
  total_calls_ = 0;
  records_.clear();
}

}
}
