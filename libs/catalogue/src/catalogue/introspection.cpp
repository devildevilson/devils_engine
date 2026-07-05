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
    out += args[i].printable ? args[i].value : "<opaque>";
  }
  return out;
}

}

call_decision trace_introspection::enter(const call_info& info) {
  utils::info("enter '{}'", info.function_name);
  return call_decision::execute;
}

void trace_introspection::exit(const call_info& info, const uint64_t elapsed_mcs) {
  utils::info("exited '{}', took {} mcs", info.function_name, elapsed_mcs);
}

void trace_introspection::skipped(const call_info& info) {
  utils::info("skipped '{}'", info.function_name);
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

}
}
