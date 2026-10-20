#include "node_scope.h"

#include <algorithm>
#include <format>
#include <stdexcept>
#include <string>

namespace gn07 {

namespace ds = devils_script;

namespace {

const location_graph& graph_of(const node_ref n) {
  return *n.view->graph;
}

const node_kind& kind_of(const node_ref n) {
  return n.view->arch->kinds[n.view->graph->nodes[n.index].kind];
}

int64_t node_depth(const node_ref n) {
  return graph_of(n).depth[n.index];
}

double node_depth_share(const node_ref n) {
  const auto& g = graph_of(n);
  return g.max_depth > 0 ? double(g.depth[n.index]) / double(g.max_depth) : 0.0;
}

int64_t node_secret_depth(const node_ref n) {
  return graph_of(n).secret_depth[n.index];
}

int64_t node_route_length(const node_ref n) {
  const auto& g = graph_of(n);
  return g.route_steps[n.index] * g.cell;
}

int64_t node_subtree(const node_ref n) {
  return graph_of(n).subtree[n.index];
}

bool node_dead_end(const node_ref n) {
  const auto& g = graph_of(n);
  return n.index != 0 && n.index != g.secret_entrance && g.neighbours[n.index].size() == 1;
}

bool node_on_secret_way(const node_ref n) {
  return graph_of(n).on_secret_way[n.index] != 0;
}

bool node_hidden_entry(const node_ref n) {
  return graph_of(n).nodes[n.index].entry == passage::hidden;
}

bool node_has_tag(const node_ref n, const std::string_view tag) {
  if (!n.view->arch->knows_tag(tag)) {
    throw std::runtime_error(std::format("predicate: tag '{}' is not declared by archetype '{}'",
      tag, n.view->arch->name));
  }
  const auto& tags = kind_of(n).tags;
  return std::find(tags.begin(), tags.end(), tag) != tags.end();
}

bool node_kind_is(const node_ref n, const std::string_view kind) {
  if (n.view->arch->kind_index(kind) == no_node) {
    throw std::runtime_error(std::format("predicate: kind '{}' is not declared by archetype '{}'",
      kind, n.view->arch->name));
  }
  return kind_of(n).name == kind;
}

bool node_function_is(const node_ref n, const std::string_view function) {
  if (!n.view->arch->knows_function(function)) {
    throw std::runtime_error(std::format("predicate: function '{}' is not declared by archetype '{}'",
      function, n.view->arch->name));
  }
  return kind_of(n).function == function;
}

bool node_at_side(const node_ref n, const std::string_view name) {
  const side wanted = side_from_string(name);
  if (wanted == side::none) {
    throw std::runtime_error(std::format("predicate: '{}' is not a side (north/east/south/west)", name));
  }
  return graph_of(n).boundary[n.index] == wanted;
}

// Факт — свойство локации, а не узла: у всех узлов один ответ. Неизвестное имя факта — не
// ошибка, а «нет»: набор фактов приходит сверху и у разных локаций разный.
bool node_has_fact(const node_ref n, const std::string_view name, const std::string_view value) {
  if (n.view->facts == nullptr) {
    return false;
  }
  for (const auto& f : *n.view->facts) {
    if (f.name == name) {
      return f.value == value;
    }
  }
  return false;
}

ds::system::options make_options() {
  ds::system::options opts;
  // Бросок отсюда библиотека глушит внутри parse(tavl::parser); отказ ловится по возврату.
  opts.error = [](const std::string& message) { throw std::runtime_error("devils_script: " + message); };
  opts.warning = [](const std::string&) {};
  return opts;
}

} // namespace

script_env::script_env() : sys_(make_options()) {
  sys_.init_basic_functions();
  sys_.init_math();
  sys_.register_function<&node_depth>("depth");
  sys_.register_function<&node_depth_share>("depth_share");
  sys_.register_function<&node_secret_depth>("secret_depth");
  sys_.register_function<&node_route_length>("route_length");
  sys_.register_function<&node_subtree>("subtree");
  sys_.register_function<&node_dead_end>("dead_end");
  sys_.register_function<&node_on_secret_way>("on_secret_way");
  sys_.register_function<&node_hidden_entry>("hidden_entry");
  sys_.register_function<&node_has_tag>("has_tag");
  sys_.register_function<&node_kind_is>("kind_is");
  sys_.register_function<&node_function_is>("function_is");
  sys_.register_function<&node_at_side>("at_side");
  sys_.register_function<&node_has_fact>("has_fact");
}

// devils_script (с 1.4.0) ставит ВСЕ блоки парсера, корень включительно, в data-режим: для него
// `{}` — список выражений, а не объект с именованными строками. Но документ архетипа читает свой
// потоковый ридер, которому нужны именованные строки, поэтому режимы возвращаются к обычным для
// tavl, а data-режим включается только на блок, отданный ds (compile_predicate).
void script_env::configure_parser(tavl::parser& parser) const {
  sys_.configure_parser(parser);
  parser.set_default_block_modes(tavl::parse_mode::object_like, tavl::parse_mode::tuple_like,
    tavl::parse_mode::data_driven);
}

ds::container script_env::compile_predicate(const std::string_view name, tavl::parser& parser) const {
  ds::container program;
  ds::system::parse_context ctx;
  constexpr auto data = tavl::parse_mode::data_driven;
  parser.override_next_block_modes(data, data, data);
  const auto [event, error] = sys_.parse<bool, node_ref>(name, parser, ctx, program);
  // Предикат без скобок блока не открывает: переопределение не должно достаться следующему объекту.
  parser.clear_pending_block_modes();
  if (error.is_critical()) {
    throw std::runtime_error(std::format("predicate '{}': devils_script parse failed ({}) at {}:{}",
      name, tavl::to_string(error.type), error.span.line, error.span.column));
  }
  return program;
}

ds::container script_env::compile_text(const std::string_view name, const std::string_view text) const {
  return sys_.parse<bool, node_ref>(name, text);
}

bool script_env::evaluate(const ds::container& program, const location_view& view, const uint32_t node) const {
  vm_.clear();
  vm_.set_arg(0, node_ref{&view, node});
  program.process(&vm_);
  if (!vm_.is_return<bool>()) {
    throw std::runtime_error("predicate did not return a boolean");
  }
  return vm_.get_return<bool>();
}

} // namespace gn07
