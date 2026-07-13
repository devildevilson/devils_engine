#include "function.h"

namespace devils_engine {
namespace act {
namespace detail {
value to_value(const bool v) noexcept {
  return value::of(v);
}

value to_value(const real_t v) noexcept {
  return value::of(v);
}

value to_value(const utils::id v) noexcept {
  return value::strv(v);
}

value to_value(const entity_id v) noexcept {
  return value::of(v);
}

bool ds_type(const std::string_view type, const std::string_view expected) noexcept {
  return type == expected;
}

bool value_from_ds(const std::string_view type, const ds::stack_element& el, value& out) {
  if (type == utils::type_name<bool>()) {
    out = value::of(el.get<bool>());
    return true;
  }
  if (type == utils::type_name<int8_t>()) {
    out = value::of(int64_t(el.get<int8_t>()));
    return true;
  }
  if (type == utils::type_name<int16_t>()) {
    out = value::of(int64_t(el.get<int16_t>()));
    return true;
  }
  if (type == utils::type_name<int32_t>()) {
    out = value::of(int64_t(el.get<int32_t>()));
    return true;
  }
  if (type == utils::type_name<int64_t>()) {
    out = value::of(el.get<int64_t>());
    return true;
  }
  if (type == utils::type_name<uint8_t>()) {
    out = value::of(int64_t(el.get<uint8_t>()));
    return true;
  }
  if (type == utils::type_name<uint16_t>()) {
    out = value::of(int64_t(el.get<uint16_t>()));
    return true;
  }
  if (type == utils::type_name<uint32_t>()) {
    out = value::of(int64_t(el.get<uint32_t>()));
    return true;
  }
  if (type == utils::type_name<uint64_t>()) {
    out = value::of(int64_t(el.get<uint64_t>()));
    return true;
  }
  if (type == utils::type_name<float>()) {
    out = value::of(real_t(el.get<float>()));
    return true;
  }
  if (type == utils::type_name<double>()) {
    out = value::of(real_t(el.get<double>()));
    return true;
  }
  if (type == utils::type_name<entity_id>()) {
    out = value::of(el.get<entity_id>());
    return true;
  }
  return false;
}

void bind_call(const ds::script_container& program, const call_context& call, ds::context* vm) {
  for (const auto& a : call.arguments()) {
    for (auto i = size_t{0}; i < program.args.size(); ++i) {
      if (utils::string_hash(program.get_arg_name(i)) != a.name) {
        continue;
      }
      value_to_ds(a.data, program.args[i].type, [vm, i](const auto& v) {
        vm->set_arg(i, v);
      });
      break;
    }
  }

  for (const auto& l : call.lists()) {
    for (auto i = size_t{0}; i < program.lists.size(); ++i) {
      if (utils::string_hash(program.get_list_name(i)) != l.name || i >= vm->lists.size()) {
        continue;
      }
      auto& dst = vm->lists[i];
      dst.clear();
      dst.reserve(l.values.size());
      for (const auto& v : l.values) {
        ds::stack_element el{};
        if (value_to_ds(v, program.lists[i].type, [&el](const auto& x) {
              el.set(x);
            })) {
          dst.push_back(el);
        }
      }
      break;
    }
  }
}

void collect_call(const ds::script_container& program, call_context& call, const ds::context* vm) {
  for (auto i = size_t{0}; i < program.args.size(); ++i) {
    value out;
    if (value_from_ds(vm->arg_type(i), vm->args_stack.element(i), out)) {
      call.argument(program.get_arg_name(i)) = out;
    }
  }

  for (auto i = size_t{0}; i < program.lists.size() && i < vm->lists.size(); ++i) {
    auto& dst = call.list(program.get_list_name(i)).values;
    dst.clear();
    dst.reserve(vm->lists[i].size());
    for (const auto& el : vm->lists[i]) {
      value out;
      if (value_from_ds(program.lists[i].type, el, out)) {
        dst.push_back(out);
      }
    }
  }
}
} // namespace detail
} // namespace act
} // namespace devils_engine
