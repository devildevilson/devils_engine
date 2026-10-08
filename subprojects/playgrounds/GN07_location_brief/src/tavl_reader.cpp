#include "tavl_reader.h"

#include <charconv>
#include <format>
#include <stdexcept>

namespace gn07 {

using ev_t = tavl::event_type;

namespace {

// Служебные события, которые не несут значения и пропускаются между элементами.
bool is_filler(const ev_t type) noexcept {
  return type == ev_t::row_begin || type == ev_t::row_end || type == ev_t::empty_row ||
    type == ev_t::got_comment;
}

} // namespace

tavl_reader::tavl_reader(tavl::parser& parser, std::string source_name)
  : parser_(parser), source_(std::move(source_name)) {}

tavl::event tavl_reader::next() {
  const auto [event, error] = parser_.poll_event();
  if (error.is_critical()) {
    fail(std::format("tavl error '{}' at {}:{}", tavl::to_string(error.type), error.span.line,
      error.span.column));
  }
  if (event.type == ev_t::not_enought_data) {
    fail("unexpected end of data");
  }
  return event;
}

tavl::event tavl_reader::peek() {
  return parser_.peek();
}

std::string tavl_reader::where() {
  const auto event = parser_.peek();
  return std::format("{}:{}:{}", source_, event.token.span.line, event.token.span.column);
}

void tavl_reader::fail(const std::string_view message) {
  throw std::runtime_error(std::format("{}: {}", where(), message));
}

void tavl_reader::read_rows(const ev_t terminator, const field_fn& field) {
  for (;;) {
    const auto event = peek();
    if (event.type == terminator) {
      next();
      return;
    }
    if (is_filler(event.type)) {
      next();
      continue;
    }
    if (event.type != ev_t::got_row_identifier) {
      fail(std::format("expected a field name, got '{}'", tavl::to_string(event.type)));
    }
    const auto name_event = next();
    const std::string name = parser_.to_string(name_event.token);
    if (peek().type != ev_t::got_row_operator) {
      fail(std::format("field '{}' has no '='", name));
    }
    next();
    field(name);
  }
}

void tavl_reader::read_document(const field_fn& field) {
  read_rows(ev_t::eof, field);
}

void tavl_reader::read_object(const field_fn& field) {
  if (next().type != ev_t::object_begin) {
    fail("expected '{'");
  }
  read_rows(ev_t::object_end, field);
}

void tavl_reader::read_array(const std::function<void()>& element) {
  if (next().type != ev_t::array_begin) {
    fail("expected '['");
  }
  for (;;) {
    const auto event = peek();
    if (event.type == ev_t::array_end) {
      next();
      return;
    }
    if (is_filler(event.type)) {
      next();
      continue;
    }
    element();
  }
}

std::string tavl_reader::read_string() {
  const auto event = next();
  if (event.type != ev_t::got_token) {
    fail(std::format("expected a value, got '{}'", tavl::to_string(event.type)));
  }
  std::string text = parser_.to_string(event.token);
  if (text.size() >= 2 && (text.front() == '"' || text.front() == '\'') && text.back() == text.front()) {
    text = text.substr(1, text.size() - 2);
  }
  return text;
}

int64_t tavl_reader::read_int() {
  const std::string text = read_string();
  int64_t value = 0;
  const auto [end, error] = std::from_chars(text.data(), text.data() + text.size(), value);
  if (error != std::errc() || end != text.data() + text.size()) {
    fail(std::format("'{}' is not an integer", text));
  }
  return value;
}

std::vector<std::string> tavl_reader::read_string_list() {
  std::vector<std::string> out;
  read_array([&] { out.push_back(read_string()); });
  return out;
}

std::vector<int64_t> tavl_reader::read_int_list() {
  std::vector<int64_t> out;
  read_array([&] { out.push_back(read_int()); });
  return out;
}

void tavl_reader::read_range(int64_t& min, int64_t& max) {
  const auto values = read_int_list();
  if (values.size() != 2 || values[0] > values[1]) {
    fail("expected a range [min, max] with min <= max");
  }
  min = values[0];
  max = values[1];
}

int64_t tavl_reader::read_fixed(const int64_t scale) {
  const std::string text = read_string();
  size_t i = 0;
  const bool negative = !text.empty() && text[0] == '-';
  if (negative) {
    ++i;
  }
  int64_t whole = 0;
  int64_t fraction = 0;
  int64_t unit = scale;
  bool digits = false;
  bool point = false;
  for (; i < text.size(); ++i) {
    const char c = text[i];
    if (c == '.' && !point) {
      point = true;
      continue;
    }
    if (c < '0' || c > '9') {
      fail(std::format("'{}' is not a number", text));
    }
    digits = true;
    if (!point) {
      whole = whole * 10 + (c - '0');
    } else {
      unit /= 10;
      if (unit == 0) {
        fail(std::format("'{}' has more fractional digits than 1/{} allows", text, scale));
      }
      fraction += (c - '0') * unit;
    }
  }
  if (!digits) {
    fail(std::format("'{}' is not a number", text));
  }
  const int64_t value = whole * scale + fraction;
  return negative ? -value : value;
}

void tavl_reader::read_fixed_range(const int64_t scale, int64_t& min, int64_t& max) {
  std::vector<int64_t> values;
  read_array([&] { values.push_back(read_fixed(scale)); });
  if (values.size() != 2 || values[0] > values[1]) {
    fail("expected a range [min, max] with min <= max");
  }
  min = values[0];
  max = values[1];
}

void tavl_reader::skip_value() {
  size_t depth = 0;
  for (;;) {
    const auto event = peek();
    const bool begin = event.type == ev_t::object_begin || event.type == ev_t::array_begin ||
      event.type == ev_t::tuple_begin || event.type == ev_t::pair_begin;
    const bool end = event.type == ev_t::object_end || event.type == ev_t::array_end ||
      event.type == ev_t::tuple_end || event.type == ev_t::pair_end;
    if (depth == 0 && (event.type == ev_t::row_end || end || event.type == ev_t::eof)) {
      return;
    }
    next();
    if (begin) {
      ++depth;
    } else if (end) {
      --depth;
      if (depth == 0) {
        return;
      }
    }
  }
}

} // namespace gn07
