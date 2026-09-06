#include "devils_engine/originator/script_translate.h"

#include <algorithm>
#include <format>
#include <ranges>

#include <devils_script/script_ast.h>
#include <devils_script/system.h>

#include "devils_engine/originator/device_form.h"
#include "devils_engine/utils/core.h"

// Реализация перевода `devils_script` -> GLSL: обход AST, который отдаёт сам ds.
//
// ПОРЯДОК ОБХОДА ЗНАЧИМ, и это главная ловушка файла. Перевод не чистая функция: попутно он нумерует
// аргументы программы (от этого зависит раскладка push-константы) и соли мест вызова случайности.
// Поэтому ветви и аргументы переводятся ОТДЕЛЬНЫМИ ОПЕРАТОРАМИ в явном порядке — порядок вычисления
// аргументов одного вызова в C++ не определён, и собранная as-is раскладка зависела бы от
// компилятора (у gcc список выходил перевёрнутым, и тест это поймал). Собирать готовый текст можно в
// любом порядке, переводить — только вперёд.
//
// ВЫРАЖЕНИЕ ОСТАЁТСЯ ВЫРАЖЕНИЕМ, а всё, что имеет имя, живёт в ПРЕАМБУЛЕ тела: сохранённые слоты,
// перекрытые аргументы, веса ветвлений и сравниваемое значение `switch`. Отсюда же следует запрет на
// запись изнутри ветки — локаль вычисляется до того, как ветка выбрана.
//
// ЧТО НЕ ПЕРЕВОДИТСЯ — ОТКАЗ С ПРИЧИНОЙ, и причина у каждого своя. Общее «не поддерживается» здесь не
// годится: одно упирается в динамическую память, другое в область видимости, третье вообще не про
// счёт.

namespace devils_engine {
namespace originator {

namespace {
namespace ds = devils_script;

// Функции `ds`, у которых имя в GLSL то же самое. Словарь совпадает не случайно: математика ds это
// буквально имена GLSL. Здесь зафиксирована только арность — ради сообщения со ссылкой на текст ds.
struct direct_function {
  std::string_view name;
  uint32_t min_args;
  uint32_t max_args;
};

constexpr direct_function direct_functions[] = {
  {"max", 2, 2},        {"min", 2, 2},   {"abs", 1, 1},        {"ceil", 1, 1},
  {"floor", 1, 1},      {"round", 1, 1}, {"trunc", 1, 1},      {"exp", 1, 1},
  {"sqrt", 1, 1},       {"inversesqrt", 1, 1},                 {"sin", 1, 1},
  {"cos", 1, 1},        {"asin", 1, 1},  {"acos", 1, 1},       {"tan", 1, 1},
  {"atan", 1, 2},       {"sign", 1, 1},  {"fma", 3, 3},        {"fract", 1, 1},
  {"mix", 3, 3},        {"clamp", 3, 3}, {"smoothstep", 3, 3}, {"step", 2, 2},
};

// Что не переводится и ПОЧЕМУ. Причина у каждого своя, и общее «не поддерживается» здесь не годится:
// одно упирается в динамическую память, другое в область видимости, третье вообще не про счёт.
struct refusal {
  std::string_view name;
  std::string_view reason;
};

constexpr refusal refusals[] = {
  {"list", "a list needs dynamic memory per element, which a shader invocation does not have"},
  {"add_to", "a list needs dynamic memory per element, which a shader invocation does not have"},
  {"is_in", "a list needs dynamic memory per element, which a shader invocation does not have"},
  {"remove_from", "a list needs dynamic memory per element, which a shader invocation does not have"},
  {"ctx_save_as", "it saves the enclosing SCOPE, and the only scope a program has is its own element"},
  {"ctx_set_as", "it saves the enclosing SCOPE, and the only scope a program has is its own element"},
  {"assert", "diagnostics do not enter a queue"},
  {"trace", "diagnostics do not enter a queue"},
  {"execute", "a sub-script would need its own translation and its own frame"},
};

// Равенство у `ds` над числами — сравнение с допуском (`raweqd`), а не побитовое. Константа
// повторена здесь НАРОЧНО и ровно та же: два определения равенства расходились бы ровно на границе,
// то есть там, где решение и принимается.
constexpr std::string_view ds_epsilon = "0.000001";

// Имя, под которым сохранённый слот или перекрытый аргумент живёт в шейдере. Приставка обязательна:
// `ctx_save = { index = ... }` не запрещено, а `index` в шейдере уже занят свёрткой индекса.
std::string saved_local(const std::string_view& name) {
  return std::format("saved_{}", name);
}

std::string overridden_local(const std::string_view& name) {
  return std::format("set_{}", name);
}

struct translator {
  const tavl::parser* parser = nullptr;
  std::string_view program_name;
  std::span<const translated_field> inputs;
  script_program::result_kind kind = script_program::result_kind::number;
  std::vector<std::string>* arguments = nullptr;
  // Локали шейдера, объявленные ДО записи результата: сохранённые слоты, перекрытые аргументы и
  // временные значения ветвлений. Выражение остаётся выражением, а всё, что имеет имя, живёт здесь.
  std::string* prelude = nullptr;

  std::vector<std::string> saved;      // слоты `ctx_save`, в порядке объявления
  std::vector<std::string> overridden; // имена, которые `ctx_set` перекрыл локалью
  uint32_t sites = 0;                  // мест вызова случайности: соль каждого следующего
  uint32_t temporaries = 0;
  // Глубина ВЕТВЛЕНИЯ. Локаль поднимается в преамбулу и вычисляется всегда, поэтому запись изнутри
  // ветки означала бы, что она случилась и тогда, когда ветка не выбрана. Такая запись отклоняется.
  uint32_t branch_depth = 0;

  std::string_view text(const tavl::token& token) const {
    return parser->content(token.span);
  }

  [[noreturn]] void refuse(const tavl::token& token, const std::string_view& what) const {
    utils::error{}("originator script '{}' @ {}:{}: {}", program_name, token.span.line, token.span.column, what);
  }

  std::string declare(const std::string_view& name, const std::string_view& type, const std::string& value) {
    prelude->append(std::format("  {} {} = {};\n", type, name, value));
    return std::string(name);
  }

  std::string temporary(const std::string& value, const std::string_view& type = "float") {
    return declare(std::format("tmp_{}", temporaries++), type, value);
  }

  // Соль места вызова. У `ds` её выдаёт эмиттер при компиляции, и в AST её нет вовсе — здесь она
  // выводится из ПОРЯДКА обхода. Порядок обхода детерминирован (каждый переход выписан явно), а
  // значит два перевода одного текста дают одни и те же соли.
  std::string chance_value() {
    return std::format("originator_chance(index, {}u)", sites++);
  }

  // Аргумент `ctx:arg:имя` становится push-константой. Порядок объявления — порядок первой встречи,
  // и он же уезжает наружу: хост обязан выкладывать байты в нём, а не догадываться.
  std::string argument(const std::string_view& path, const tavl::token& token) {
    static constexpr std::string_view prefix = "ctx:arg:";
    const auto name = path.substr(prefix.size());
    if (name.empty()) {
      refuse(token, "ctx:arg: names no argument");
    }

    // `ctx_set` перекрывает аргумент локалью, и чтение ПОСЛЕ него обязано видеть локаль: у `ds`
    // `ctx_set` пишет ровно в тот слот, откуда читает `ctx:arg:`.
    if (std::find(overridden.begin(), overridden.end(), name) != overridden.end()) {
      return overridden_local(name);
    }

    const auto found = std::find(arguments->begin(), arguments->end(), name);
    if (found == arguments->end()) {
      arguments->emplace_back(name);
    }
    return std::format("args.arg_{}", name);
  }

  // Чтение сохранённого слота. Слот, который на этом пути ещё не записан, — отказ: у `ds` это ошибка
  // типа («Trying to load unsaved value»), а в шейдере было бы чтение неинициализированной локали.
  std::string saved_value(const std::string_view& path, const tavl::token& token) const {
    static constexpr std::string_view prefix = "ctx:saved:";
    const auto name = path.substr(prefix.size());
    if (std::find(saved.begin(), saved.end(), name) == saved.end()) {
      refuse(token, std::format("'{}' is read before any ctx_save wrote it", path));
    }
    return saved_local(name);
  }

  // Чтение поля своего элемента. Никакого другого обращения к данным у программы нет — она
  // структурно pointwise, и именно поэтому один её вызов это одна инвокация без синхронизации.
  std::string field(const std::string_view& name, const tavl::token& token) const {
    for (size_t i = 0; i < inputs.size(); ++i) {
      if (inputs[i].name != name) continue;
      const auto type = device_type_name(inputs[i].base);
      return type == "float" ? std::format("in_{}_at(index)", i)
                             : std::format("float(in_{}_at(index))", i);
    }

    refuse(token, std::format("'{}' is not one of the bound fields: a program is compiled against the names of "
                              "its bindings, and an unbound name cannot become anything",
                              name));
  }

  // Значение ребёнка, у которого значение ОБЯЗАНО быть: `ctx_save` и `ctx_set` это операторы, а не
  // выражения, и попытка сложить их с числом должна называться, а не превращаться в пустое место.
  std::string required(const tavl::node_view node, const std::string_view& what) {
    auto result = value(node);
    if (result.empty()) {
      refuse(node.root().token, std::format("{} is a statement and produces no value", what));
    }
    return result;
  }

  // Имя пары `имя = ...`, если узел ею является. Оператор проверяется, потому что `a + b` — тоже
  // пара с токеном слева: без проверки `a` читалось бы как имя.
  std::string_view pair_name(const tavl::node_view node) const {
    if (node.root().type != tavl::node_type::pair || node.size() != 2) return {};
    const auto op = node.root().token.exists() ? text(node.root().token) : std::string_view{};
    if (!op.empty() && op != "=" && op != "?=") return {};
    const auto head = node.child(0);
    if (head.root().type != tavl::node_type::token) return {};
    return text(head.root().token);
  }

  // Ищет `имя = ...` среди детей блока.
  bool named(const tavl::node_view node, const std::string_view& name, tavl::node_view& out) const {
    for (size_t i = 0; i < node.size(); ++i) {
      const auto child = node.child(i);
      if (pair_name(child) != name) continue;
      out = child.child(1);
      return true;
    }
    return false;
  }

  // Ветка конструкции. Ветки можно называть (`шум = { weight = 1, ... }`), и тогда веткой является
  // правая часть пары, а не она сама.
  tavl::node_view clause_of(const tavl::node_view node) const {
    return pair_name(node).empty() ? node : node.child(1);
  }

  static bool is_block(const tavl::node_view node) noexcept {
    const auto type = node.root().type;
    return type == tavl::node_type::object || type == tavl::node_type::row ||
           type == tavl::node_type::array || type == tavl::node_type::tuple;
  }

  // УСЛОВИЕ СВОРАЧИВАЕТСЯ ПО И ВСЕГДА, независимо от того, что возвращает сама программа: так его
  // сворачивает `ds` (`dispatch_node(..., "AND")`), и у числовой программы условие из двух строк
  // иначе стало бы их СУММОЙ.
  std::string condition(const tavl::node_view node, const std::string_view& what) {
    const auto declared = kind;
    kind = script_program::result_kind::predicate;
    auto result = required(node, what);
    kind = declared;
    return result;
  }

  std::string value(const tavl::node_view node);
  std::string block(const tavl::node_view node, const std::span<const std::string_view>& skip = {});
  std::string call(const std::string_view& callee, const tavl::node_view args, const tavl::token& token);
  std::string binary(const std::string_view& op, const tavl::node_view node);

  std::string select_block(const tavl::node_view args, const tavl::token& token);
  std::string sequence_block(const tavl::node_view args, const tavl::token& token);
  std::string switch_block(const tavl::node_view args, const tavl::token& token);
  std::string random_block(const tavl::node_view args, const tavl::token& token);
  std::string save_block(const tavl::node_view args, const tavl::token& token, const bool into_argument);
};

std::string translator::binary(const std::string_view& op, const tavl::node_view node) {
  const auto& token = node.root().token;

  if (node.size() == 1) {
    const auto operand = required(node.child(0), std::format("the operand of '{}'", op));
    if (op == "-") return std::format("(-{})", operand);
    if (op == "+") return operand;
    if (op == "not") return std::format("(!{})", operand);
    refuse(token, std::format("unary operator '{}' has no shader form", op));
  }

  if (node.size() != 2) {
    refuse(token, std::format("operator '{}' got {} operands", op, node.size()));
  }

  const auto left = required(node.child(0), std::format("the left operand of '{}'", op));
  const auto right = required(node.child(1), std::format("the right operand of '{}'", op));

  if (op == "+" || op == "-" || op == "*" || op == "/" || op == "<" || op == ">" || op == "<=" || op == ">=") {
    return std::format("({} {} {})", left, op, right);
  }
  if (op == "%") {
    // У float остатка в GLSL нет как оператора, а `mod` считает то же, что `%` у ds над числами.
    return std::format("mod({}, {})", left, right);
  }
  // Равенство С ДОПУСКОМ — то же, что делает `ds`: побитовое `==` над плавающими считало бы РАЗНЫМИ
  // числа, которые ds считает равными, и расходились бы пути ровно на границе правила.
  if (op == "==") return std::format("(abs({} - {}) < {})", left, right, ds_epsilon);
  if (op == "!=") return std::format("(abs({} - {}) >= {})", left, right, ds_epsilon);
  if (op == "and") return std::format("({} && {})", left, right);
  if (op == "or") return std::format("({} || {})", left, right);

  refuse(token, std::format("operator '{}' has no shader form", op));
}

// `select = { { condition = c, v }, ..., { else } }`: первое истинное условие побеждает, последний
// блок — иначе-ветка. Собирается ОБРАТНЫМ проходом по уже переведённым частям, а не рекурсией по
// хвосту: перевод попутно нумерует аргументы и соли случайности, поэтому переводить обязан ВПЕРЁД, а
// собирать текст можно как угодно.
std::string translator::select_block(const tavl::node_view args, const tavl::token& token) {
  if (args.size() < 2) {
    refuse(token, "'select' needs at least one conditional block and an else block");
  }

  std::vector<std::pair<std::string, std::string>> clauses;
  std::string otherwise;

  for (size_t i = 0; i < args.size(); ++i) {
    const auto clause = clause_of(args.child(i));
    const bool last = i + 1 == args.size();

    tavl::node_view test;
    if (!named(clause, "condition", test)) {
      if (!last) {
        refuse(clause.root().token, "every block of 'select' except the last one needs a 'condition'");
      }
      otherwise = required(clause, "the else block of 'select'");
      continue;
    }
    if (last) {
      refuse(clause.root().token, "the last block of 'select' is the else branch and must not carry a 'condition'");
    }

    // Условие первой ветки вычисляется всегда, остальные — только если предыдущие не сработали.
    // Ветвлением поэтому считается всё, кроме него.
    branch_depth += uint32_t(i != 0);
    auto taken_if = condition(test, "a 'condition' of 'select'");
    branch_depth -= uint32_t(i != 0);

    ++branch_depth;
    static constexpr std::string_view skip[] = {"condition"};
    auto taken = block(clause, skip);
    --branch_depth;

    clauses.emplace_back(std::move(taken_if), std::move(taken));
  }

  if (otherwise.empty()) {
    refuse(token, "'select' has no else block, and a shader has nowhere to return no value to");
  }

  auto result = std::move(otherwise);
  for (auto it = clauses.rbegin(); it != clauses.rend(); ++it) {
    result = std::format("({} ? {} : {})", it->first, it->second, result);
  }
  return result;
}

// `sequence = { { condition = c, v }, ... }`: блоки считаются, пока условия истинны, и первое ложное
// обрывает всю цепочку. Накопитель тот же, что у `ds`: сумма у числовой программы, И у предикатной.
std::string translator::sequence_block(const tavl::node_view args, const tavl::token& token) {
  if (args.size() == 0) {
    refuse(token, "'sequence' with no blocks computes nothing");
  }

  const bool predicate = kind == script_program::result_kind::predicate;
  const std::string_view identity = predicate ? "true" : "0.0";
  const std::string_view combinator = predicate ? " && " : " + ";

  std::vector<std::pair<std::string, std::string>> clauses;
  for (size_t i = 0; i < args.size(); ++i) {
    const auto clause = clause_of(args.child(i));

    tavl::node_view test;
    if (!named(clause, "condition", test)) {
      refuse(clause.root().token, "every block of 'sequence' needs a 'condition'");
    }

    branch_depth += uint32_t(i != 0);
    auto taken_if = condition(test, "a 'condition' of 'sequence'");
    branch_depth -= uint32_t(i != 0);

    ++branch_depth;
    static constexpr std::string_view skip[] = {"condition"};
    auto taken = block(clause, skip);
    --branch_depth;

    clauses.emplace_back(std::move(taken_if), std::move(taken));
  }

  std::string result(identity);
  for (auto it = clauses.rbegin(); it != clauses.rend(); ++it) {
    result = std::format("({} ? ({}{}{}) : {})", it->first, it->second, combinator, result, identity);
  }
  return result;
}

// `switch = { value = subject, { value = case, body }, ... }`: пачка сравнений с одним и тем же
// значением. Значение считается ОДИН раз и живёт локалью — иначе оно вычислялось бы столько раз,
// сколько написано веток.
std::string translator::switch_block(const tavl::node_view args, const tavl::token& token) {
  tavl::node_view subject_node;
  if (!named(args, "value", subject_node)) {
    refuse(token, "'switch' needs a top-level 'value' to compare against");
  }

  const auto subject = temporary(required(subject_node, "the 'value' of 'switch'"));

  std::vector<std::pair<std::string, std::string>> cases;
  for (size_t i = 0; i < args.size(); ++i) {
    // Сам `value = ...` это не ветка, а сравниваемое значение.
    if (pair_name(args.child(i)) == "value") continue;
    const auto clause = clause_of(args.child(i));
    if (!is_block(clause)) {
      refuse(clause.root().token, "a case of 'switch' is a block with its own 'value'");
    }

    tavl::node_view case_node;
    if (!named(clause, "value", case_node)) {
      refuse(clause.root().token, "every case block of 'switch' needs its own 'value'");
    }

    branch_depth += uint32_t(!cases.empty());
    auto label = required(case_node, "the 'value' of a 'switch' case");
    branch_depth -= uint32_t(!cases.empty());

    ++branch_depth;
    static constexpr std::string_view skip[] = {"value"};
    auto body = block(clause, skip);
    --branch_depth;

    cases.emplace_back(std::move(label), std::move(body));
  }

  if (cases.empty()) {
    refuse(token, "'switch' needs at least one case block");
  }

  // НЕСОВПАВШИЙ `switch` ОТДАЁТ НУЛЬ, и это объявленное расхождение с CPU: у `ds` ветки по умолчанию
  // нет вовсе, и не совпавший `switch` не кладёт на стек ничего. Ноль здесь — определённое поведение
  // вместо неопределённого, а не перевод чужого правила.
  std::string result = kind == script_program::result_kind::predicate ? "false" : "0.0";
  for (auto it = cases.rbegin(); it != cases.rend(); ++it) {
    result = std::format("(abs({} - {}) < {} ? {} : {})", subject, it->first, ds_epsilon, it->second, result);
  }
  return result;
}

// `random = { { weight = w, v }, ... }`: выбор ветки по весам. Веса считаются ВСЕ и до выбора —
// ровно как у `ds`, где они накапливаются префиксной суммой на стеке, — а `chance` умножается на
// итог. Поэтому веса становятся локалями: они вычисляются один раз и безусловно.
std::string translator::random_block(const tavl::node_view args, const tavl::token& token) {
  if (args.size() == 0) {
    refuse(token, "'random' with no blocks has nothing to choose from");
  }

  std::vector<std::string> thresholds;
  thresholds.reserve(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    const auto clause = clause_of(args.child(i));
    tavl::node_view weight;
    if (!named(clause, "weight", weight)) {
      refuse(clause.root().token, "every block of 'random' needs a 'weight'");
    }
    auto value = required(weight, "a 'weight' of 'random'");
    // Порог — префиксная сумма, а не сам вес: сравнение идёт с накопленной границей.
    thresholds.push_back(temporary(thresholds.empty() ? std::move(value)
                                                      : std::format("{} + {}", thresholds.back(), value)));
  }

  const auto pick = temporary(std::format("{} * {}", chance_value(), thresholds.back()));

  std::vector<std::string> values;
  values.reserve(args.size());
  for (size_t i = 0; i < args.size(); ++i) {
    ++branch_depth;
    static constexpr std::string_view skip[] = {"weight"};
    values.push_back(block(clause_of(args.child(i)), skip));
    --branch_depth;
  }

  // Последняя ветка безусловна: `pick` меньше полной суммы весов по построению, поэтому сравнение с
  // последним порогом ничего не решает, а лишний тернарник требовал бы значения, которого нет.
  auto result = values.back();
  for (size_t i = values.size() - 1; i > 0; --i) {
    result = std::format("({} <= {} ? {} : {})", pick, thresholds[i - 1], values[i - 1], result);
  }
  return result;
}

// `ctx_save = { имя = значение }` и `ctx_set = { имя = значение }` — ОПЕРАТОРЫ, а не выражения: они
// объявляют локаль шейдера и не отдают значения. Разница между ними ровно та же, что у `ds`:
// `ctx_save` заводит свой слот (`ctx:saved:имя`), `ctx_set` пишет в слот АРГУМЕНТА (`ctx:arg:имя`),
// то есть перекрывает пришедшее в push-константе значение.
std::string translator::save_block(const tavl::node_view args, const tavl::token& token, const bool into_argument) {
  const std::string_view name_of = into_argument ? "ctx_set" : "ctx_save";
  if (args.size() == 0) {
    refuse(token, std::format("'{}' needs at least one 'name = value' pair", name_of));
  }
  // ЗАПИСЬ ИЗ ВЕТКИ ОТКЛОНЯЕТСЯ. Локаль поднимается в преамбулу и вычисляется всегда, поэтому запись
  // внутри невыбранной ветки состоялась бы всё равно — то есть программа считала бы не то, что
  // написано. Доказательства доминирования у перевода нет, поэтому граница проведена по синтаксису.
  if (branch_depth != 0) {
    refuse(token, std::format("'{}' inside a branch does not translate: a shader local is computed before the "
                              "branch is chosen, so the write would happen even when the branch is not taken",
                              name_of));
  }

  for (size_t i = 0; i < args.size(); ++i) {
    const auto pair = args.child(i);
    if (pair.root().type != tavl::node_type::pair || pair.size() != 2) {
      refuse(pair.root().token, std::format("'{}' takes 'name = value' pairs", name_of));
    }
    const auto head = pair.child(0);
    if (head.root().type != tavl::node_type::token) {
      refuse(pair.root().token, std::format("'{}' takes 'name = value' pairs", name_of));
    }

    const auto name = text(head.root().token);
    const auto stored = required(pair.child(1), std::format("the value of '{} = ...'", name));

    auto& table = into_argument ? overridden : saved;
    const auto local = into_argument ? overridden_local(name) : saved_local(name);
    if (std::find(table.begin(), table.end(), name) != table.end()) {
      // Второе сохранение того же имени — присваивание: локаль уже объявлена.
      prelude->append(std::format("  {} = {};\n", local, stored));
      continue;
    }

    table.emplace_back(name);
    declare(local, "float", stored);
  }

  return {};
}

std::string translator::call(const std::string_view& callee,
                             const tavl::node_view args,
                             const tavl::token& token) {
  for (const auto& entry : refusals) {
    if (entry.name == callee) {
      refuse(token, std::format("'{}' does not translate: {}", callee, entry.reason));
    }
  }

  // `value_or = { условие, тогда, иначе }` — тройная форма, та самая, из которой в GN01 собрано
  // правило биома вложением. Другие арности ds допускает, и они переводятся тем же тернарником, но
  // непроверенная догадка о порядке аргументов означала бы правило, считающее не то.
  if (callee == "value_or") {
    if (args.size() != 3) {
      refuse(token, std::format("'value_or' with {} arguments is not translated; the three-argument form "
                                "(condition, then, else) is",
                                args.size()));
    }
    // Ветви переводятся ОТДЕЛЬНЫМИ ОПЕРАТОРАМИ, а не аргументами одного вызова: порядок вычисления
    // аргументов функции в C++ не определён, а перевод не чистая функция — он попутно собирает список
    // аргументов программы и соли случайности, и от их порядка зависит раскладка push-константы.
    // Собранная as-is раскладка зависела бы от компилятора: у gcc список выходил перевёрнутым.
    const auto test = condition(args.child(0), "the condition of 'value_or'");
    ++branch_depth;
    const auto positive = required(args.child(1), "the 'then' branch of 'value_or'");
    const auto negative = required(args.child(2), "the 'else' branch of 'value_or'");
    --branch_depth;
    return std::format("({} ? {} : {})", test, positive, negative);
  }

  if (callee == "select") return select_block(args, token);
  if (callee == "sequence") return sequence_block(args, token);
  if (callee == "switch") return switch_block(args, token);
  if (callee == "random") return random_block(args, token);
  if (callee == "ctx_save") return save_block(args, token, false);
  if (callee == "ctx_set") return save_block(args, token, true);

  // `chance` — ЗНАЧЕНИЕ, а не вызов: пишется `chance < 0.5`. Форма с аргументами отклоняется, потому
  // что `ds` их молча игнорирует, и написанное число выглядело бы как порог, которым оно не является.
  if (callee == "chance") {
    if (args.root().type == tavl::node_type::token || args.size() != 0) {
      refuse(token, "'chance' takes no arguments: it is a value in [0, 1), written as `chance < 0.5`");
    }
    return chance_value();
  }

  // `rndmix`/`rndmix1` — хеш САМИХ ЗНАЧЕНИЙ, а не элемента: от зерна прохода они не зависят ни здесь,
  // ни на CPU, поэтому одно и то же число даёт одно и то же значение везде.
  if (callee == "rndmix1" || callee == "rndmix") {
    const size_t expected = callee == "rndmix1" ? 1 : 2;
    if (args.size() != expected) {
      refuse(token, std::format("'{}' takes {} arguments, got {}", callee, expected, args.size()));
    }
    const auto first = required(args.child(0), std::format("the argument of '{}'", callee));
    if (expected == 1) {
      return std::format("originator_mix1({})", first);
    }
    const auto second = required(args.child(1), std::format("the second argument of '{}'", callee));
    return std::format("originator_mix2({}, {})", first, second);
  }

  // `inv` — единственная арифметическая функция ds без тёзки в GLSL.
  if (callee == "inv") {
    if (args.size() != 1) {
      refuse(token, std::format("'inv' takes one argument, got {}", args.size()));
    }
    return std::format("(1.0 / {})", required(args.child(0), "the argument of 'inv'"));
  }

  for (const auto& entry : direct_functions) {
    if (entry.name != callee) continue;
    if (args.size() < entry.min_args || args.size() > entry.max_args) {
      refuse(token, std::format("'{}' takes {}..{} arguments, got {}", callee, entry.min_args, entry.max_args,
                                args.size()));
    }

    // Цикл, а не свёртка аргументов в один вызов, по той же причине: порядок здесь ЗНАЧИМ, потому
    // что перевод попутно нумерует аргументы программы.
    std::string result(callee);
    result.push_back('(');
    for (size_t i = 0; i < args.size(); ++i) {
      if (i != 0) result.append(", ");
      const auto argument = required(args.child(i), std::format("an argument of '{}'", callee));
      result.append(argument);
    }
    result.push_back(')');
    return result;
  }

  refuse(token, std::format("'{}' is not a function the translator knows; the vocabulary is the GLSL-named "
                            "arithmetic of ds, value_or, inv, the branch blocks (select, sequence, switch, "
                            "random), randomness (chance, rndmix, rndmix1) and the locals (ctx_save, ctx_set)",
                            callee));
}

// Блок, который НЕ является списком аргументов вызова, — это комбинатор: у числовой программы
// строки складываются, у предикатной сходятся по И. Так их и соединяет сам ds, и в переводе это
// правило то же, а не своё. Операторы (`ctx_save`) значения не дают и в свёртку не входят.
std::string translator::block(const tavl::node_view node, const std::span<const std::string_view>& skip) {
  if (node.size() == 0) {
    refuse(node.root().token, "an empty block computes nothing");
  }

  std::vector<std::string> parts;
  for (size_t i = 0; i < node.size(); ++i) {
    const auto child = node.child(i);

    if (std::find(skip.begin(), skip.end(), pair_name(child)) != skip.end()) continue;

    auto part = value(child);
    if (part.empty()) continue; // оператор: он объявил локаль, а слагаемым не является
    parts.push_back(std::move(part));
  }

  if (parts.empty()) {
    refuse(node.root().token, "a block of statements alone computes nothing");
  }
  if (parts.size() == 1) {
    return parts.front();
  }

  const std::string_view combinator = kind == script_program::result_kind::predicate ? " && " : " + ";
  std::string result = "(";
  for (size_t i = 0; i < parts.size(); ++i) {
    if (i != 0) result.append(combinator);
    result.append(parts[i]);
  }
  result.push_back(')');
  return result;
}

std::string translator::value(const tavl::node_view node) {
  const auto& root = node.root();

  switch (root.type) {
    case tavl::node_type::token: {
      const auto content = text(root.token);
      switch (root.token.type) {
        case tavl::token_type::number_int:
        case tavl::token_type::number_uint:
        case tavl::token_type::number_float:
          // Через float(): целочисленный литерал в GLSL это int, и во float-выражении он бы менял тип.
          return std::format("float({})", content);
        case tavl::token_type::boolean: return std::string(content);
        case tavl::token_type::identifier:
          if (content == "chance") return chance_value();
          return field(content, root.token);
        case tavl::token_type::unrecognized: {
          // Путь области (`a.b:c`) приезжает одним нераспознанным токеном. Осмысленных путей здесь
          // два: аргумент прохода и слот, который программа сама же и сохранила.
          if (content.starts_with("ctx:arg:")) {
            return argument(content, root.token);
          }
          if (content.starts_with("ctx:saved:")) {
            return saved_value(content, root.token);
          }
          refuse(root.token, std::format("scope path '{}' has no shader form: a program can read the fields of "
                                         "its own element, its arguments and its own saved slots, and nothing else",
                                         content));
        }
        default:
          refuse(root.token, std::format("token '{}' of type '{}' has no shader form", content,
                                         tavl::to_string(root.token.type)));
      }
    }

    case tavl::node_type::pair: {
      const auto op = root.token.exists() ? text(root.token) : std::string_view{};
      // `=` и `?=` у ds это операторы ВЫЗОВА, а не присваивания: `f = {...}` означает `f(...)`.
      // Вызов с пустым оператором — та же запись через скобки. Всё остальное — математика.
      if (op.empty() || op == "=" || op == "?=") {
        if (node.size() != 2) {
          refuse(root.token, std::format("a call needs a callee and its arguments, got {} parts", node.size()));
        }
        const auto callee = node.child(0);
        if (callee.root().type != tavl::node_type::token) {
          refuse(root.token, "the callee of a call is not a name");
        }
        return call(text(callee.root().token), node.child(1), callee.root().token);
      }
      return binary(op, node);
    }

    case tavl::node_type::object:
    case tavl::node_type::row:
    case tavl::node_type::array:
    case tavl::node_type::tuple:
      return block(node);

    default:
      refuse(root.token, std::format("node of type '{}' has no shader form", int(root.type)));
  }
}
} // namespace

translation translate_to_glsl(const std::string_view& name,
                             const std::string_view& source,
                             const std::span<const translated_field>& inputs,
                             const translated_field& output,
                             const script_program::result_kind kind,
                             const uint32_t group_size) {
  if (inputs.size() > max_script_inputs) {
    utils::error{}("originator script '{}': {} input fields, at most {} are supported", name, inputs.size(),
                   max_script_inputs);
  }
  if (device_type_name(output.base).empty()) {
    utils::error{}("originator script '{}': output field '{}' has storage kind '{}', which a shader buffer has no "
                   "type for; the queue takes the 32-bit kinds v, ui and i",
                   name, output.name, to_string(output.base));
  }
  for (const auto& field : inputs) {
    if (!device_type_name(field.base).empty()) continue;
    utils::error{}("originator script '{}': input field '{}' has storage kind '{}', which a shader buffer has no "
                   "type for; the queue takes the 32-bit kinds v, ui and i",
                   name, field.name, to_string(field.base));
  }
  for (const auto& field : inputs) {
    // `chance` — имя встроенного значения, и поле с таким именем сделало бы случайность невидимой:
    // программа читала бы поле там, где написана случайность, и наоборот.
    if (field.name != "chance") continue;
    utils::error{}("originator script '{}': a bound field named 'chance' cannot be told apart from the builtin "
                   "randomness of the same name",
                   name);
  }

  // Парсер настраивает САМА система ds: приоритеты и фиксность операторов объявлены в ней, и второй
  // их список разъехался бы с первым молча.
  ds::system system;
  system.init_basic_functions();
  system.init_math();

  tavl::parser parser;
  system.configure_parser(parser);

  std::vector<tavl::node> nodes;
  try {
    nodes = ds::make_script_ast(parser, source);
  } catch (const std::exception& error) {
    utils::error{}("originator script '{}': could not parse for translation: {}", name, error.what());
  }
  if (nodes.empty()) {
    utils::error{}("originator script '{}': the program is empty", name);
  }

  translation result;
  result.group_size = group_size;

  std::string prelude;

  translator emitter;
  emitter.parser = &parser;
  emitter.program_name = name;
  emitter.inputs = inputs;
  emitter.kind = kind;
  emitter.arguments = &result.arguments;
  emitter.prelude = &prelude;

  const auto expression = emitter.value(tavl::node_view{nodes});

  // Преобразование результата в род поля повторяет `store_component` НАРОЧНО: две разные записи одного
  // и того же значения дали бы два разных поля, и разница проявилась бы только на границах диапазона.
  // Верхнюю границу float32 при этом не представляет точно (`4294967295` округляется до `4294967296`),
  // и это объявленное расхождение путей на самом краю, а не незамеченная ошибка.
  std::string stored;
  const auto numeric = kind == script_program::result_kind::predicate
                         ? std::format("(({}) ? 1.0 : 0.0)", expression)
                         : std::format("({})", expression);
  if (output.base == field_base::v) {
    stored = numeric;
  } else if (output.base == field_base::ui) {
    stored = std::format("uint(clamp({}, 0.0, 4294967295.0))", numeric);
  } else {
    stored = std::format("int(clamp({}, -2147483648.0, 2147483647.0))", numeric);
  }

  // ШЕЙДЕР НЕСЁТ СВОЁ ПРОИСХОЖДЕНИЕ. Заголовок-комментарий отвечает на вопрос «как именно перевод
  // перенёс скрипт»: исходный текст ds, какой биндинг какому полю соответствует и как выложена
  // push-константа. Без этого сверять сторону хоста приходится по памяти, а перепутанный порядок
  // аргументов не выглядит ошибкой ни с той, ни с другой стороны. Комментарий бесплатен: glslc
  // снимает его при компиляции.
  //
  // Язык здесь АНГЛИЙСКИЙ, в отличие от комментариев самой библиотеки: этот текст уезжает в дамп
  // шейдера, в сообщение glslc и в отладчик, то есть читают его инструменты и люди вне проекта.
  std::string text;
  text.append(std::format("// Translated from devils_script: program '{}'.\n//\n", name));
  text.append("// SOURCE:\n");
  for (const auto line : std::views::split(source, '\n')) {
    text.append("//   ");
    text.append(std::string_view(line.begin(), line.end()));
    text.push_back('\n');
  }
  text.append("//\n// BINDINGS:\n");
  for (size_t i = 0; i < inputs.size(); ++i) {
    text.append(std::format("//   binding {} = input '{}' ({})\n", i, inputs[i].name, to_string(inputs[i].base)));
  }
  text.append(std::format("//   binding {} = output '{}' ({})\n", inputs.size(), output.name, to_string(output.base)));
  text.append("//\n// PUSH CONSTANT, byte by byte:\n");
  text.append("//   0: uint count\n//   4: uint begin\n//   8: uint extent_x\n//   12: uint extent_y\n"
              "//   16: uint seed\n");
  for (size_t i = 0; i < result.arguments.size(); ++i) {
    text.append(std::format("//   {}: float arg_{}\n", sizeof(device_call_header) + i * sizeof(float),
                            result.arguments[i]));
  }
  text.append(std::format("//\n// EXPRESSION:\n//   {}\n\n", expression));

  // ПРИВЯЗКИ СОБИРАЕТ ОБЩИЙ СБОРЩИК, а не транслятор. Иначе у перевода была бы СВОЯ шапка
  // push-константы — а у него она и была, и отличалась от инструментной: `uint count` против
  // `count, begin, extent_x, extent_y`. Пока перевод не попадал в устройственную очередь, это не
  // стреляло; попав, вызов прочитал бы `begin` вместо первого аргумента и не пожаловался бы.
  std::vector<device_binding> shape;
  shape.reserve(inputs.size() + 1);
  for (const auto& binding : inputs) {
    device_binding declared;
    declared.base = binding.base;
    shape.push_back(declared);
  }
  device_binding written;
  written.base = output.base;
  written.writable = true;
  shape.push_back(written);

  std::vector<device_param> params;
  params.reserve(result.arguments.size());
  for (const auto& argument : result.arguments) {
    // Имя аргумента ds попадает в шейдер с приставкой: `ctx:arg:` не запрещает называться `min` или
    // `index`, а имя поля push-константы обязано не столкнуться ни со встроенной функцией, ни с
    // локальной переменной свёртки индекса.
    params.push_back(device_param{argument, 0.0, std::format("arg_{}", argument)});
  }

  // Форма ВЫДАЁТСЯ здесь и только здесь: чужое тело попадает на устройство ровно как переведённая
  // программа `devils_script`, и никак иначе. Локали идут перед записью — в них живут сохранённые
  // слоты, перекрытые аргументы и веса ветвлений.
  auto body = prelude + std::format("  out_0_set(index, {});\n", stored);
  text.append(build_device_shader(shape, params, body, group_size));
  result.form = translated_form(translation_authority::key(), std::move(body), std::move(params), {});

  result.source = std::move(text);
  return result;
}

} // namespace originator
} // namespace devils_engine
