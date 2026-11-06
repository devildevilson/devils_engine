#include "devils_engine/demiurg/module_profile.h"

#include <algorithm>
#include <set>
#include <tavl/deserialize.h>
#include <tavl/serialize.h>
#include "devils_engine/utils/atomic_file.h"
#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

namespace devils_engine::demiurg {
namespace {
// The general reflect reader is intentionally permissive about unknown fields and some
// scalar-to-container inputs. Profiles are persisted configuration: validate their shape first.
void validate_text(std::string_view text, const std::filesystem::path& source) {
  using ev = tavl::event_type;
  using tok = tavl::token_type;
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();
  auto next = [&]() {
    for (;;) {
      const auto [event, error] = parser.poll_event();
      if (error.type != tavl::error_type::no_error) utils::error{}("Module profile '{}': TAVL {} at {}:{}", source.generic_string(), tavl::to_string(error.type), error.span.line + 1, error.span.column + 1);
      if (event.type == ev::row_begin || event.type == ev::got_comment || event.type == ev::empty_row) continue;
      return event;
    }
  };
  auto expect = [&](const tavl::event& event, bool valid, const char* what) {
    if (!valid) utils::error{}("Module profile '{}': expected {} at {}:{}", source.generic_string(), what, event.token.span.line + 1, event.token.span.column + 1);
  };
  std::set<std::string> fields;
  for (;;) {
    auto event = next();
    if (event.type == ev::eof) break;
    if (event.type == ev::row_end) continue;
    expect(event, event.type == ev::got_row_identifier, "profile field");
    const auto field = parser.to_string(event.token);
    expect(event, (field == "version" || field == "modules") && fields.insert(field).second, "unique version/modules field");
    event = next();
    expect(event, event.type == ev::got_row_operator && parser.content(event.token.span) == "=", "'=' operator");
    event = next();
    if (field == "version") {
      expect(event, event.type == ev::got_token && (event.token.type == tok::number_int || event.token.type == tok::number_uint), "integer version");
    } else {
      expect(event, event.type == ev::array_begin, "modules array");
      for (;;) {
        event = next();
        if (event.type == ev::array_end) break;
        if (event.type == ev::row_end) continue;
        expect(event, event.type == ev::object_begin, "module entry object");
        std::set<std::string> members;
        for (;;) {
          event = next();
          if (event.type == ev::object_end) break;
          if (event.type == ev::row_end) continue;
          expect(event, event.type == ev::got_row_identifier, "module entry field");
          const auto member = parser.to_string(event.token);
          expect(event, (member == "path" || member == "hash" || member == "file_date") && members.insert(member).second, "unique path/hash/file_date field");
          event = next();
          expect(event, event.type == ev::got_row_operator && parser.content(event.token.span) == "=", "'=' operator");
          event = next();
          expect(event, event.type == ev::got_token && (event.token.type == tok::identifier || event.token.type == tok::singlequote_string || event.token.type == tok::doublequote_string), "string value");
          event = next();
          expect(event, event.type == ev::row_end, "end of entry field");
        }
        expect(event, members.contains("path"), "module path");
      }
    }
    event = next();
    expect(event, event.type == ev::row_end, "end of profile field");
  }
  if (!fields.contains("version") || !fields.contains("modules")) utils::error{}("Module profile '{}': version and modules fields are required", source.generic_string());
}
void validate(const module_profile& profile, const std::filesystem::path& source) {
  if (profile.version != 1) utils::error{}("Module profile '{}': unsupported version {}, expected 1", source.generic_string(), profile.version);
  std::set<std::string> paths;
  for (size_t i = 0; i < profile.modules.size(); ++i) {
    const auto& entry = profile.modules[i];
    const std::filesystem::path path(entry.path);
    const auto normalized = path.lexically_normal();
    if (entry.path.empty() || entry.path.find('\0') != std::string::npos || entry.path.find_first_of("\\:") != std::string::npos ||
        path.is_absolute() || path.has_root_name() || normalized.empty() || normalized == "." ||
        *normalized.begin() == "..") {
      utils::error{}("Module profile '{}': entry {} has invalid root-relative path '{}'", source.generic_string(), i, entry.path);
    }
    auto key = normalized.generic_string();
    while (key.ends_with('/'))
      key.pop_back();
    if (!paths.insert(key).second) utils::error{}("Module profile '{}': duplicate path '{}' at entry {}", source.generic_string(), entry.path, i);
    if (!entry.hash.empty() && (entry.hash.size() != 64 || !std::all_of(entry.hash.begin(), entry.hash.end(), [](char c) {
                                  return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
                                }))) utils::error{}("Module profile '{}': entry {} hash must be empty or lowercase SHA256", source.generic_string(), i);
  }
}
} // namespace

std::filesystem::path module_profile_path(const std::filesystem::path& root, const std::string_view name) {
  const std::filesystem::path file(name);
  if (name.empty() || name.find('\0') != std::string_view::npos || name.find_first_of("/\\:") != std::string_view::npos || file == "." || file == "..") {
    utils::error{}("Module profile name '{}' must be a filename inside the modules root", name);
  }
  if (file.extension() == ".json") utils::error{}("JSON module profile '{}' is no longer supported; use scripts/convert_module_profile.py", name);
  return root / (file.extension() == ".tavl" ? file : std::filesystem::path(std::string(name) + ".tavl"));
}

module_profile read_module_profile(const std::filesystem::path& path) {
  if (path.extension() == ".json") utils::error{}("JSON module profile '{}' is no longer supported; use scripts/convert_module_profile.py", path.generic_string());
  if (!file_io::exists(path.generic_string())) {
    auto legacy = path;
    legacy.replace_extension(".json");
    if (file_io::exists(legacy.generic_string())) utils::error{}("Module profile '{}' is still JSON; convert it with scripts/convert_module_profile.py", legacy.generic_string());
    utils::error{}("Module profile '{}' does not exist", path.generic_string());
  }
  const auto text = file_io::read(path.generic_string());
  validate_text(text, path);
  tavl::parser parser;
  parser.add_default_operator();
  parser.flush(text);
  parser.finish();
  tavl::ct_context context;
  module_profile profile;
  tavl::deserialize(parser, context, profile);
  for (const auto& diagnostic : context.diagnostics) {
    if (diagnostic.error.is_critical() || (diagnostic.error.type == tavl::error_type::warn_missing_field &&
                                           (diagnostic.field == "version" || diagnostic.field == "modules" || diagnostic.field == "path"))) {
      utils::error{}("Module profile '{}': TAVL {} for field '{}' at {}:{}", path.generic_string(), tavl::to_string(diagnostic.error.type), diagnostic.field,
                     diagnostic.error.span.line + 1, diagnostic.error.span.column + 1);
    }
  }
  validate(profile, path);
  return profile;
}

void write_module_profile(const std::filesystem::path& path, const module_profile& profile) {
  if (path.extension() == ".json") utils::error{}("JSON module profile '{}' is no longer supported; use a .tavl destination", path.generic_string());
  validate(profile, path);
  std::string text;
  if (!tavl::serialize(profile, text)) utils::error{}("Could not serialize module profile '{}'", path.generic_string());
  const auto result = file_io::atomic_replace(path.generic_string(), std::string_view(text));
  if (!result) utils::error{}("Could not write module profile '{}' at {}: {}", path.generic_string(), file_io::atomic_file_stage_name(result.error().stage), result.error().code.message());
}
} // namespace devils_engine::demiurg
