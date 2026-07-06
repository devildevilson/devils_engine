#include "resource_manifest.h"

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/string-utils.hpp"

namespace devils_engine {
namespace demiurg {
namespace {

std::string trim_copy(const std::string_view value) {
  return std::string(utils::string::trim(value));
}

size_t count_newlines(const std::string_view value) {
  size_t count = 0;
  for (const char c : value) {
    if (c == '\n') count += 1;
  }
  return count;
}

struct tavl_section_view {
  std::string_view content;
  uint32_t index = 0;
  uint32_t start_line = 0;
  size_t offset = SIZE_MAX;
  size_t size = 0;
};

std::string strip_inline_comment(std::string_view value) {
  const size_t comment = value.find("//");
  if (comment != std::string_view::npos) value = value.substr(0, comment);
  return trim_copy(value);
}

std::string parse_name_field(const std::string_view section) {
  size_t pos = 0;
  while (pos < section.size()) {
    size_t end = section.find('\n', pos);
    if (end == std::string_view::npos) end = section.size();

    std::string_view line = utils::string::trim(section.substr(pos, end - pos));
    pos = end + (end < section.size() ? 1 : 0);

    if (line.empty() || line.starts_with("//")) continue;
    if (!line.starts_with("name")) continue;

    line.remove_prefix(4);
    line = utils::string::trim(line);
    if (line.empty() || line.front() != '=') continue;
    line.remove_prefix(1);
    std::string value = strip_inline_comment(line);
    if (value.size() >= 2 && ((value.front() == '"' && value.back() == '"') || (value.front() == '\'' && value.back() == '\''))) {
      value = value.substr(1, value.size() - 2);
    }
    return value;
  }

  return {};
}

std::vector<tavl_section_view> split_tavl_sections(const std::string_view content) {
  static constexpr std::string_view separator = "//---";

  std::vector<tavl_section_view> sections;
  size_t pos = 0;
  size_t line = 1;
  uint32_t index = 0;
  while (pos <= content.size()) {
    const size_t sep = content.find(separator, pos);
    const size_t end = sep == std::string_view::npos ? content.size() : sep;
    std::string_view raw = content.substr(pos, end - pos);
    size_t raw_offset = pos;
    uint32_t start_line = static_cast<uint32_t>(line);
    while (!raw.empty() && (raw.front() == ' ' || raw.front() == '\t' || raw.front() == '\r' || raw.front() == '\n')) {
      if (raw.front() == '\n') start_line += 1;
      raw.remove_prefix(1);
      raw_offset += 1;
    }
    raw = utils::string::trim(raw);
    if (!raw.empty()) sections.push_back(tavl_section_view{raw, index, start_line, raw_offset, raw.size()});
    if (sep == std::string_view::npos) break;
    line += count_newlines(content.substr(pos, sep + separator.size() - pos));
    pos = sep + separator.size();
    index += 1;
  }
  return sections;
}

}

bool append_tavl_list_candidates(
  std::vector<resource_candidate>& out,
  const resource_candidate& base,
  const std::string_view content
) {
  if (base.ext != "tavl" || content.find("//---") == std::string_view::npos) return false;

  const auto sections = split_tavl_sections(content);

  for (const auto& section : sections) {
    resource_candidate candidate = base;
    candidate.aliases.clear();
    candidate.list_index = section.index;
    candidate.list_start_line = section.start_line;
    candidate.list_offset = section.offset;
    candidate.list_size = section.size;
    candidate.list_section = std::string(section.content);
    candidate.list_name = parse_name_field(section.content);

    const std::string index_id = base.id + ":" + std::to_string(section.index);
    if (!candidate.list_name.empty()) {
      candidate.id = base.id + ":" + candidate.list_name;
      if (candidate.id != index_id) candidate.aliases.push_back(index_id);
    } else {
      candidate.id = index_id;
    }

    out.push_back(std::move(candidate));
  }

  return true;
}

}
}
