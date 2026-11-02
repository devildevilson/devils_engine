#include "devils_engine/originator/script_column.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <format>

#include <devils_script/script_ast.h>
#include <devils_script/system.h>

#include "devils_engine/utils/core.h"

// Колоночный исполнитель: AST ds -> линейная программа над регистрами-столбцами.
//
// РЕГИСТР — указатель на n значений. Столбец входа читается на месте (без копии), скаляр и постоянная
// разворачиваются в столбец один раз на прогон (постоянные — один раз на программу), вычисленное
// значение пишется в свой буфер. Регистры не переиспользуются: программы формулы короткие, а
// однократное присваивание делает порядок операций очевидным.
//
// Операция — один проход по пакету без ветвлений внутри: ветвление считает обе ветви и выбирает по
// маске. Отсюда векторизация компилятором и цена, почти не зависящая от того, какая ветвь выпала.

namespace devils_engine {
namespace originator {

namespace {
namespace ds = devils_script;

enum class op_code : uint8_t {
  add, sub, mul, div, mod, neg,
  lt, gt, le, ge, eq, ne, land, lor, lnot,
  choose, // c ? a : b
  min, max, abs, floor, ceil, round, trunc, sqrt, isqrt, exp, sin, cos, tan, asin, acos, atan, atan2, sign,
  fma, fract, mix, clamp, smoothstep, step, inv
};

struct op {
  op_code code;
  uint16_t dst = 0, a = 0, b = 0, c = 0;
};

struct named_function {
  std::string_view name;
  op_code code;
  uint32_t args;
};

constexpr named_function functions[] = {
  {"min", op_code::min, 2},     {"max", op_code::max, 2},       {"abs", op_code::abs, 1},
  {"floor", op_code::floor, 1}, {"ceil", op_code::ceil, 1},     {"round", op_code::round, 1},
  {"trunc", op_code::trunc, 1}, {"sqrt", op_code::sqrt, 1},     {"inversesqrt", op_code::isqrt, 1},
  {"exp", op_code::exp, 1},     {"sin", op_code::sin, 1},       {"cos", op_code::cos, 1},
  {"tan", op_code::tan, 1},     {"asin", op_code::asin, 1},     {"acos", op_code::acos, 1},
  {"sign", op_code::sign, 1},   {"fma", op_code::fma, 3},       {"fract", op_code::fract, 1},
  {"mix", op_code::mix, 3},     {"clamp", op_code::clamp, 3},   {"smoothstep", op_code::smoothstep, 3},
  {"step", op_code::step, 2},   {"inv", op_code::inv, 1},
};

// Равенство у ds над числами — с допуском (`raweqd`); та же постоянная, что у перевода в GLSL.
constexpr double ds_epsilon = 0.000001;

} // namespace

struct column_program::implementation {
  enum class source : uint8_t { column, scalar, constant, computed };
  struct reg {
    source kind = source::computed;
    uint32_t index = 0; // столбец, скаляр, постоянная или буфер вычисленного
  };

  std::string name;
  std::vector<reg> regs;
  std::vector<op> ops;
  std::vector<std::array<double, lanes>> constants;
  std::vector<uint8_t> used_columns;
  size_t computed = 0;
  uint16_t result = 0;
  size_t scalar_count = 0;
};

namespace {

struct compiler {
  const tavl::parser* parser = nullptr;
  column_program::implementation* out = nullptr;
  std::span<const std::string> columns;
  std::span<const std::string> scalars;
  std::vector<std::pair<std::string, uint16_t>> saved;
  uint32_t branch_depth = 0;
  static constexpr uint16_t none = UINT16_MAX;

  std::string_view text(const tavl::token& token) const { return parser->content(token.span); }

  [[noreturn]] void refuse(const tavl::token& token, const std::string_view& what) const {
    utils::error{}("originator column script '{}' @ {}:{}: {}", out->name, token.span.line, token.span.column, what);
  }

  uint16_t add_reg(const column_program::implementation::source kind, const uint32_t index) {
    if (out->regs.size() >= none) utils::error{}("originator column script '{}': too many registers", out->name);
    out->regs.push_back({kind, index});
    return uint16_t(out->regs.size() - 1);
  }

  uint16_t constant(const double value) {
    for (size_t r = 0; r < out->regs.size(); ++r) {
      const auto& reg = out->regs[r];
      if (reg.kind == column_program::implementation::source::constant && out->constants[reg.index][0] == value) {
        return uint16_t(r);
      }
    }
    std::array<double, column_program::lanes> filled;
    filled.fill(value);
    out->constants.push_back(filled);
    return add_reg(column_program::implementation::source::constant, uint32_t(out->constants.size() - 1));
  }

  uint16_t emit(const op_code code, const uint16_t a, const uint16_t b = 0, const uint16_t c = 0) {
    const uint16_t dst = add_reg(column_program::implementation::source::computed, uint32_t(out->computed++));
    out->ops.push_back({code, dst, a, b, c});
    return dst;
  }

  uint16_t name(const std::string_view& id, const tavl::token& token) {
    for (size_t i = 0; i < columns.size(); ++i) {
      if (columns[i] != id) continue;
      out->used_columns[i] = 1;
      for (size_t r = 0; r < out->regs.size(); ++r) {
        if (out->regs[r].kind == column_program::implementation::source::column && out->regs[r].index == i) {
          return uint16_t(r);
        }
      }
      return add_reg(column_program::implementation::source::column, uint32_t(i));
    }
    for (size_t i = 0; i < scalars.size(); ++i) {
      if (scalars[i] != id) continue;
      for (size_t r = 0; r < out->regs.size(); ++r) {
        if (out->regs[r].kind == column_program::implementation::source::scalar && out->regs[r].index == i) {
          return uint16_t(r);
        }
      }
      return add_reg(column_program::implementation::source::scalar, uint32_t(i));
    }
    refuse(token, std::format("'{}' is neither a column nor a scalar of this program", id));
  }

  uint16_t required(const tavl::node_view node, const std::string_view& what) {
    const uint16_t r = value(node);
    if (r == none) refuse(node.root().token, std::format("{} is a statement and produces no value", what));
    return r;
  }

  std::string_view pair_name(const tavl::node_view node) const {
    if (node.root().type != tavl::node_type::pair || node.size() != 2) return {};
    const auto op = node.root().token.exists() ? text(node.root().token) : std::string_view{};
    if (!op.empty() && op != "=" && op != "?=") return {};
    const auto head = node.child(0);
    if (head.root().type != tavl::node_type::token) return {};
    return text(head.root().token);
  }

  bool named(const tavl::node_view node, const std::string_view& id, tavl::node_view& found) const {
    for (size_t i = 0; i < node.size(); ++i) {
      if (pair_name(node.child(i)) != id) continue;
      found = node.child(i).child(1);
      return true;
    }
    return false;
  }

  tavl::node_view clause_of(const tavl::node_view node) const {
    return pair_name(node).empty() ? node : node.child(1);
  }

  static bool is_block(const tavl::node_view node) noexcept {
    const auto type = node.root().type;
    return type == tavl::node_type::object || type == tavl::node_type::row || type == tavl::node_type::array ||
           type == tavl::node_type::tuple;
  }

  // Блок-комбинатор: строки числовой программы складываются (так их сворачивает ds), операторы
  // (`ctx_save`) значения не дают.
  uint16_t block(const tavl::node_view node, const std::span<const std::string_view> skip = {}) {
    uint16_t sum = none;
    for (size_t i = 0; i < node.size(); ++i) {
      const auto child = node.child(i);
      if (std::find(skip.begin(), skip.end(), pair_name(child)) != skip.end()) continue;
      const uint16_t part = value(child);
      if (part == none) continue;
      sum = sum == none ? part : emit(op_code::add, sum, part);
    }
    if (sum == none) refuse(node.root().token, "a block of statements alone computes nothing");
    return sum;
  }

  uint16_t binary(const std::string_view& op, const tavl::node_view node) {
    const auto& token = node.root().token;
    if (node.size() == 1) {
      const uint16_t a = required(node.child(0), std::format("the operand of '{}'", op));
      if (op == "-") return emit(op_code::neg, a);
      if (op == "+") return a;
      if (op == "not") return emit(op_code::lnot, a);
      refuse(token, std::format("unary operator '{}' has no column form", op));
    }
    if (node.size() != 2) refuse(token, std::format("operator '{}' got {} operands", op, node.size()));
    const uint16_t a = required(node.child(0), std::format("the left operand of '{}'", op));
    const uint16_t b = required(node.child(1), std::format("the right operand of '{}'", op));
    if (op == "+") return emit(op_code::add, a, b);
    if (op == "-") return emit(op_code::sub, a, b);
    if (op == "*") return emit(op_code::mul, a, b);
    if (op == "/") return emit(op_code::div, a, b);
    if (op == "%") return emit(op_code::mod, a, b);
    if (op == "<") return emit(op_code::lt, a, b);
    if (op == ">") return emit(op_code::gt, a, b);
    if (op == "<=") return emit(op_code::le, a, b);
    if (op == ">=") return emit(op_code::ge, a, b);
    if (op == "==") return emit(op_code::eq, a, b);
    if (op == "!=") return emit(op_code::ne, a, b);
    if (op == "and") return emit(op_code::land, a, b);
    if (op == "or") return emit(op_code::lor, a, b);
    refuse(token, std::format("operator '{}' has no column form", op));
  }

  uint16_t select_block(const tavl::node_view args, const tavl::token& token) {
    if (args.size() < 2) refuse(token, "'select' needs at least one conditional block and an else block");
    std::vector<std::pair<uint16_t, uint16_t>> clauses;
    uint16_t otherwise = none;
    for (size_t i = 0; i < args.size(); ++i) {
      const auto clause = clause_of(args.child(i));
      const bool last = i + 1 == args.size();
      tavl::node_view test;
      if (!named(clause, "condition", test)) {
        if (!last) refuse(clause.root().token, "every block of 'select' except the last one needs a 'condition'");
        ++branch_depth;
        otherwise = block(clause);
        --branch_depth;
        continue;
      }
      if (last) refuse(clause.root().token, "the last block of 'select' is the else branch, without a 'condition'");
      const uint16_t taken_if = required(test, "a 'condition' of 'select'");
      ++branch_depth;
      static constexpr std::string_view skip[] = {"condition"};
      const uint16_t taken = block(clause, skip);
      --branch_depth;
      clauses.emplace_back(taken_if, taken);
    }
    uint16_t result = otherwise;
    for (auto it = clauses.rbegin(); it != clauses.rend(); ++it) result = emit(op_code::choose, it->first, it->second, result);
    return result;
  }

  uint16_t sequence_block(const tavl::node_view args, const tavl::token& token) {
    if (args.size() == 0) refuse(token, "'sequence' with no blocks computes nothing");
    std::vector<std::pair<uint16_t, uint16_t>> clauses;
    for (size_t i = 0; i < args.size(); ++i) {
      const auto clause = clause_of(args.child(i));
      tavl::node_view test;
      if (!named(clause, "condition", test)) refuse(clause.root().token, "every block of 'sequence' needs a 'condition'");
      const uint16_t taken_if = required(test, "a 'condition' of 'sequence'");
      ++branch_depth;
      static constexpr std::string_view skip[] = {"condition"};
      const uint16_t taken = block(clause, skip);
      --branch_depth;
      clauses.emplace_back(taken_if, taken);
    }
    const uint16_t zero = constant(0.0);
    uint16_t result = zero;
    for (auto it = clauses.rbegin(); it != clauses.rend(); ++it) {
      result = emit(op_code::choose, it->first, emit(op_code::add, it->second, result), zero);
    }
    return result;
  }

  uint16_t switch_block(const tavl::node_view args, const tavl::token& token) {
    tavl::node_view subject_node;
    if (!named(args, "value", subject_node)) refuse(token, "'switch' needs a top-level 'value' to compare against");
    const uint16_t subject = required(subject_node, "the 'value' of 'switch'");
    std::vector<std::pair<uint16_t, uint16_t>> cases;
    for (size_t i = 0; i < args.size(); ++i) {
      if (pair_name(args.child(i)) == "value") continue;
      const auto clause = clause_of(args.child(i));
      if (!is_block(clause)) refuse(clause.root().token, "a case of 'switch' is a block with its own 'value'");
      tavl::node_view case_node;
      if (!named(clause, "value", case_node)) refuse(clause.root().token, "every case of 'switch' needs a 'value'");
      const uint16_t label = required(case_node, "the 'value' of a 'switch' case");
      ++branch_depth;
      static constexpr std::string_view skip[] = {"value"};
      const uint16_t body = block(clause, skip);
      --branch_depth;
      cases.emplace_back(emit(op_code::eq, subject, label), body);
    }
    if (cases.empty()) refuse(token, "'switch' needs at least one case block");
    uint16_t result = constant(0.0);
    for (auto it = cases.rbegin(); it != cases.rend(); ++it) result = emit(op_code::choose, it->first, it->second, result);
    return result;
  }

  uint16_t save_block(const tavl::node_view args, const tavl::token& token) {
    if (args.size() == 0) refuse(token, "'ctx_save' needs at least one 'name = value' pair");
    if (branch_depth != 0) {
      refuse(token, "'ctx_save' inside a branch has no column form: both branches are computed, so the write would "
                    "happen even when the branch is not taken");
    }
    for (size_t i = 0; i < args.size(); ++i) {
      const auto pair = args.child(i);
      if (pair.root().type != tavl::node_type::pair || pair.size() != 2 ||
          pair.child(0).root().type != tavl::node_type::token) {
        refuse(pair.root().token, "'ctx_save' takes 'name = value' pairs");
      }
      const auto id = std::string(text(pair.child(0).root().token));
      const uint16_t stored = required(pair.child(1), std::format("the value of '{} = ...'", id));
      auto found = std::find_if(saved.begin(), saved.end(), [&](const auto& s) { return s.first == id; });
      if (found != saved.end()) found->second = stored;
      else saved.emplace_back(id, stored);
    }
    return none;
  }

  uint16_t call(const std::string_view& callee, const tavl::node_view args, const tavl::token& token) {
    if (callee == "value_or") {
      if (args.size() != 3) refuse(token, "'value_or' has a column form with three arguments (condition, then, else)");
      const uint16_t test = required(args.child(0), "the condition of 'value_or'");
      ++branch_depth;
      const uint16_t positive = required(args.child(1), "the 'then' branch of 'value_or'");
      const uint16_t negative = required(args.child(2), "the 'else' branch of 'value_or'");
      --branch_depth;
      return emit(op_code::choose, test, positive, negative);
    }
    if (callee == "select") return select_block(args, token);
    if (callee == "sequence") return sequence_block(args, token);
    if (callee == "switch") return switch_block(args, token);
    if (callee == "ctx_save") return save_block(args, token);
    if (callee == "atan" && args.size() == 2) {
      const uint16_t a = required(args.child(0), "an argument of 'atan'");
      const uint16_t b = required(args.child(1), "an argument of 'atan'");
      return emit(op_code::atan2, a, b);
    }
    if (callee == "atan" && args.size() == 1) return emit(op_code::atan, required(args.child(0), "the argument of 'atan'"));
    for (const auto& f : functions) {
      if (f.name != callee) continue;
      // Аргумент-лист из одного значения приходит самим значением, а не блоком.
      const bool single = args.root().type == tavl::node_type::token || !is_block(args);
      const size_t count = single ? 1 : args.size();
      if (count != f.args) refuse(token, std::format("'{}' takes {} arguments, got {}", callee, f.args, count));
      std::array<uint16_t, 3> regs{};
      for (size_t i = 0; i < count; ++i) {
        regs[i] = required(single ? args : args.child(i), std::format("an argument of '{}'", callee));
      }
      return emit(f.code, regs[0], regs[1], regs[2]);
    }
    refuse(token, std::format("'{}' has no column form; the vocabulary is the arithmetic of ds, value_or, select, "
                              "sequence, switch and ctx_save", callee));
  }

  uint16_t value(const tavl::node_view node) {
    const auto& root = node.root();
    switch (root.type) {
      case tavl::node_type::token: {
        const auto content = text(root.token);
        switch (root.token.type) {
          case tavl::token_type::number_int:
          case tavl::token_type::number_uint:
          case tavl::token_type::number_float: return constant(std::stod(std::string(content)));
          case tavl::token_type::boolean: return constant(content == "true" ? 1.0 : 0.0);
          case tavl::token_type::identifier: return name(content, root.token);
          case tavl::token_type::unrecognized: {
            if (content.starts_with("ctx:saved:")) {
              const auto id = content.substr(10);
              for (const auto& [saved_name, reg] : saved) {
                if (saved_name == id) return reg;
              }
              refuse(root.token, std::format("'{}' is read before any ctx_save wrote it", content));
            }
            refuse(root.token, std::format("scope path '{}' has no column form: a program reads its columns, its "
                                           "scalars and its own saved slots",
                                           content));
          }
          default: refuse(root.token, std::format("token '{}' has no column form", content));
        }
      }
      case tavl::node_type::pair: {
        const auto op = root.token.exists() ? text(root.token) : std::string_view{};
        if (op.empty() || op == "=" || op == "?=") {
          if (node.size() != 2 || node.child(0).root().type != tavl::node_type::token) {
            refuse(root.token, "a call needs a callee name and its arguments");
          }
          return call(text(node.child(0).root().token), node.child(1), node.child(0).root().token);
        }
        return binary(op, node);
      }
      case tavl::node_type::object:
      case tavl::node_type::row:
      case tavl::node_type::array:
      case tavl::node_type::tuple: return block(node);
      default: refuse(root.token, "this node has no column form");
    }
  }
};

} // namespace

column_program::column_program(std::unique_ptr<implementation> state) noexcept : state_(std::move(state)) {}
column_program::~column_program() noexcept = default;

const std::string& column_program::name() const noexcept { return state_->name; }
const std::vector<uint8_t>& column_program::used_columns() const noexcept { return state_->used_columns; }

std::unique_ptr<column_program> column_program::compile(const std::string_view& name, const std::string_view& source,
                                                        const std::span<const std::string> columns,
                                                        const std::span<const std::string> scalars) {
  // Парсер настраивает сама система ds: приоритеты операторов объявлены в ней.
  ds::system system;
  system.init_basic_functions();
  system.init_math();
  tavl::parser parser;
  system.configure_parser(parser);
  std::vector<tavl::node> nodes;
  try {
    nodes = ds::make_script_ast(parser, source);
  } catch (const std::exception& error) {
    utils::error{}("originator column script '{}': could not parse: {}", name, error.what());
  }
  if (nodes.empty()) utils::error{}("originator column script '{}': the program is empty", name);

  auto state = std::make_unique<implementation>();
  state->name = name;
  state->used_columns.assign(columns.size(), 0);
  state->scalar_count = scalars.size();
  compiler c;
  c.parser = &parser;
  c.out = state.get();
  c.columns = columns;
  c.scalars = scalars;
  state->result = c.required(tavl::node_view{nodes}, "the program");
  return std::unique_ptr<column_program>(new column_program(std::move(state)));
}

void column_program::run(const size_t n, const std::span<const double* const> columns,
                         const std::span<const double> scalars, double* out) const {
  const auto& s = *state_;
  if (n > lanes) utils::error{}("originator column script '{}': {} values, a pass holds {}", s.name, n, lanes);
  if (scalars.size() < s.scalar_count) {
    utils::error{}("originator column script '{}': {} scalars given, {} declared", s.name, scalars.size(), s.scalar_count);
  }
  if (n == 1) {
    // Одна точка (лучи, проверки раскладки): регистры — числа, без столбцов и подготовки указателей.
    thread_local std::vector<double> value;
    value.resize(s.regs.size());
    for (size_t r = 0; r < s.regs.size(); ++r) {
      const auto& reg = s.regs[r];
      switch (reg.kind) {
        case implementation::source::column: value[r] = columns[reg.index][0]; break;
        case implementation::source::scalar: value[r] = scalars[reg.index]; break;
        case implementation::source::constant: value[r] = s.constants[reg.index][0]; break;
        case implementation::source::computed: break;
      }
    }
    for (const op& o : s.ops) {
      const double a = value[o.a], b = value[o.b], c = value[o.c];
      double& d = value[o.dst];
      switch (o.code) {
        case op_code::add: d = a + b; break;
        case op_code::sub: d = a - b; break;
        case op_code::mul: d = a * b; break;
        case op_code::div: d = a / b; break;
        case op_code::mod: d = a - b * std::floor(a / b); break;
        case op_code::neg: d = -a; break;
        case op_code::lt: d = a < b ? 1.0 : 0.0; break;
        case op_code::gt: d = a > b ? 1.0 : 0.0; break;
        case op_code::le: d = a <= b ? 1.0 : 0.0; break;
        case op_code::ge: d = a >= b ? 1.0 : 0.0; break;
        case op_code::eq: d = std::abs(a - b) < ds_epsilon ? 1.0 : 0.0; break;
        case op_code::ne: d = std::abs(a - b) >= ds_epsilon ? 1.0 : 0.0; break;
        case op_code::land: d = (a != 0.0 && b != 0.0) ? 1.0 : 0.0; break;
        case op_code::lor: d = (a != 0.0 || b != 0.0) ? 1.0 : 0.0; break;
        case op_code::lnot: d = a == 0.0 ? 1.0 : 0.0; break;
        case op_code::choose: d = a != 0.0 ? b : c; break;
        case op_code::min: d = std::min(a, b); break;
        case op_code::max: d = std::max(a, b); break;
        case op_code::abs: d = std::abs(a); break;
        case op_code::floor: d = std::floor(a); break;
        case op_code::ceil: d = std::ceil(a); break;
        case op_code::round: d = std::round(a); break;
        case op_code::trunc: d = std::trunc(a); break;
        case op_code::sqrt: d = std::sqrt(a); break;
        case op_code::isqrt: d = 1.0 / std::sqrt(a); break;
        case op_code::exp: d = std::exp(a); break;
        case op_code::sin: d = std::sin(a); break;
        case op_code::cos: d = std::cos(a); break;
        case op_code::tan: d = std::tan(a); break;
        case op_code::asin: d = std::asin(a); break;
        case op_code::acos: d = std::acos(a); break;
        case op_code::atan: d = std::atan(a); break;
        case op_code::atan2: d = std::atan2(a, b); break;
        case op_code::sign: d = double((a > 0.0) - (a < 0.0)); break;
        case op_code::fma: d = a * b + c; break;
        case op_code::fract: d = a - std::floor(a); break;
        case op_code::mix: d = a + (b - a) * c; break;
        case op_code::clamp: d = std::min(std::max(a, b), c); break;
        case op_code::smoothstep: {
          const double t = std::clamp((c - a) / (b - a), 0.0, 1.0);
          d = t * t * (3.0 - 2.0 * t);
          break;
        }
        case op_code::step: d = b < a ? 0.0 : 1.0; break;
        case op_code::inv: d = 1.0 / a; break;
      }
    }
    out[0] = value[s.result];
    return;
  }
  thread_local std::vector<std::array<double, lanes>> computed;
  thread_local std::vector<std::array<double, lanes>> broadcast;
  thread_local std::vector<const double*> regs;
  if (computed.size() < s.computed) computed.resize(s.computed);
  if (broadcast.size() < s.scalar_count) broadcast.resize(s.scalar_count);
  regs.assign(s.regs.size(), nullptr);
  for (size_t r = 0; r < s.regs.size(); ++r) {
    const auto& reg = s.regs[r];
    switch (reg.kind) {
      case implementation::source::column:
        if (columns.size() <= reg.index || columns[reg.index] == nullptr) {
          utils::error{}("originator column script '{}': column {} is read but not given", s.name, reg.index);
        }
        regs[r] = columns[reg.index];
        break;
      case implementation::source::scalar: {
        auto& filled = broadcast[reg.index];
        std::fill_n(filled.data(), n, scalars[reg.index]);
        regs[r] = filled.data();
        break;
      }
      case implementation::source::constant: regs[r] = s.constants[reg.index].data(); break;
      case implementation::source::computed: regs[r] = computed[reg.index].data(); break;
    }
  }
  for (const op& o : s.ops) {
    double* d = computed[s.regs[o.dst].index].data();
    const double* a = regs[o.a];
    const double* b = regs[o.b];
    const double* c = regs[o.c];
    switch (o.code) {
      case op_code::add: for (size_t l = 0; l < n; ++l) d[l] = a[l] + b[l]; break;
      case op_code::sub: for (size_t l = 0; l < n; ++l) d[l] = a[l] - b[l]; break;
      case op_code::mul: for (size_t l = 0; l < n; ++l) d[l] = a[l] * b[l]; break;
      case op_code::div: for (size_t l = 0; l < n; ++l) d[l] = a[l] / b[l]; break;
      case op_code::mod: for (size_t l = 0; l < n; ++l) d[l] = a[l] - b[l] * std::floor(a[l] / b[l]); break;
      case op_code::neg: for (size_t l = 0; l < n; ++l) d[l] = -a[l]; break;
      case op_code::lt: for (size_t l = 0; l < n; ++l) d[l] = a[l] < b[l] ? 1.0 : 0.0; break;
      case op_code::gt: for (size_t l = 0; l < n; ++l) d[l] = a[l] > b[l] ? 1.0 : 0.0; break;
      case op_code::le: for (size_t l = 0; l < n; ++l) d[l] = a[l] <= b[l] ? 1.0 : 0.0; break;
      case op_code::ge: for (size_t l = 0; l < n; ++l) d[l] = a[l] >= b[l] ? 1.0 : 0.0; break;
      case op_code::eq: for (size_t l = 0; l < n; ++l) d[l] = std::abs(a[l] - b[l]) < ds_epsilon ? 1.0 : 0.0; break;
      case op_code::ne: for (size_t l = 0; l < n; ++l) d[l] = std::abs(a[l] - b[l]) >= ds_epsilon ? 1.0 : 0.0; break;
      case op_code::land: for (size_t l = 0; l < n; ++l) d[l] = (a[l] != 0.0 && b[l] != 0.0) ? 1.0 : 0.0; break;
      case op_code::lor: for (size_t l = 0; l < n; ++l) d[l] = (a[l] != 0.0 || b[l] != 0.0) ? 1.0 : 0.0; break;
      case op_code::lnot: for (size_t l = 0; l < n; ++l) d[l] = a[l] == 0.0 ? 1.0 : 0.0; break;
      case op_code::choose: for (size_t l = 0; l < n; ++l) d[l] = a[l] != 0.0 ? b[l] : c[l]; break;
      case op_code::min: for (size_t l = 0; l < n; ++l) d[l] = std::min(a[l], b[l]); break;
      case op_code::max: for (size_t l = 0; l < n; ++l) d[l] = std::max(a[l], b[l]); break;
      case op_code::abs: for (size_t l = 0; l < n; ++l) d[l] = std::abs(a[l]); break;
      case op_code::floor: for (size_t l = 0; l < n; ++l) d[l] = std::floor(a[l]); break;
      case op_code::ceil: for (size_t l = 0; l < n; ++l) d[l] = std::ceil(a[l]); break;
      case op_code::round: for (size_t l = 0; l < n; ++l) d[l] = std::round(a[l]); break;
      case op_code::trunc: for (size_t l = 0; l < n; ++l) d[l] = std::trunc(a[l]); break;
      case op_code::sqrt: for (size_t l = 0; l < n; ++l) d[l] = std::sqrt(a[l]); break;
      case op_code::isqrt: for (size_t l = 0; l < n; ++l) d[l] = 1.0 / std::sqrt(a[l]); break;
      case op_code::exp: for (size_t l = 0; l < n; ++l) d[l] = std::exp(a[l]); break;
      case op_code::sin: for (size_t l = 0; l < n; ++l) d[l] = std::sin(a[l]); break;
      case op_code::cos: for (size_t l = 0; l < n; ++l) d[l] = std::cos(a[l]); break;
      case op_code::tan: for (size_t l = 0; l < n; ++l) d[l] = std::tan(a[l]); break;
      case op_code::asin: for (size_t l = 0; l < n; ++l) d[l] = std::asin(a[l]); break;
      case op_code::acos: for (size_t l = 0; l < n; ++l) d[l] = std::acos(a[l]); break;
      case op_code::atan: for (size_t l = 0; l < n; ++l) d[l] = std::atan(a[l]); break;
      case op_code::atan2: for (size_t l = 0; l < n; ++l) d[l] = std::atan2(a[l], b[l]); break;
      case op_code::sign: for (size_t l = 0; l < n; ++l) d[l] = double((a[l] > 0.0) - (a[l] < 0.0)); break;
      case op_code::fma: for (size_t l = 0; l < n; ++l) d[l] = a[l] * b[l] + c[l]; break;
      case op_code::fract: for (size_t l = 0; l < n; ++l) d[l] = a[l] - std::floor(a[l]); break;
      case op_code::mix: for (size_t l = 0; l < n; ++l) d[l] = a[l] + (b[l] - a[l]) * c[l]; break;
      case op_code::clamp: for (size_t l = 0; l < n; ++l) d[l] = std::min(std::max(a[l], b[l]), c[l]); break;
      case op_code::smoothstep:
        for (size_t l = 0; l < n; ++l) {
          const double t = std::clamp((c[l] - a[l]) / (b[l] - a[l]), 0.0, 1.0);
          d[l] = t * t * (3.0 - 2.0 * t);
        }
        break;
      case op_code::step: for (size_t l = 0; l < n; ++l) d[l] = b[l] < a[l] ? 0.0 : 1.0; break;
      case op_code::inv: for (size_t l = 0; l < n; ++l) d[l] = 1.0 / a[l]; break;
    }
  }
  const double* result = regs[s.result];
  std::copy_n(result, n, out);
}

} // namespace originator
} // namespace devils_engine
