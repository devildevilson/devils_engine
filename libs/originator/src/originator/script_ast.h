#pragma once

#include <expected>
#include <string>
#include <vector>
#include <devils_script/script_ast.h>

namespace devils_engine::originator::detail {
// The DS streaming interface reports syntax errors as data. Join its top-level
// rows under the same synthetic root used by the whole-string DS helper.
inline std::expected<std::vector<tavl::node>, std::string> read_script_ast(tavl::parser& parser, std::string_view source) {
  parser.clear();
  parser.flush(source);
  parser.finish();
  devils_script::script_ast_context context;
  std::vector<tavl::node> result{tavl::node{tavl::node_type::object, {}, 0}};
  std::vector<tavl::node> row;
  for (;;) {
    const auto [event, error] = devils_script::make_script_ast(parser, context, row);
    if (!error.no_error()) return std::unexpected(context.error_message);
    if (event.type == tavl::event_type::not_enought_data) return std::unexpected("incomplete script source");
    if (!row.empty()) result.insert(result.end(), row.begin() + 1, row.end());
    if (event.type == tavl::event_type::eof) break;
  }
  result.front().child_count = result.size() - 1;
  return result;
}
} // namespace devils_engine::originator::detail
