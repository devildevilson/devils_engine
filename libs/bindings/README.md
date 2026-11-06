# Lua environment and data conversion

`devils_engine::bindings_base` is the headless Lua 5.4/sol2 target. It has no Nuklear or Vulkan
dependency. `devils_engine::bindings` adds UI bindings. Include
`devils_engine/bindings/data_conversion.h` for data conversion.

```cpp
struct settings {
  std::string title;
  uint64_t seed;
  std::vector<std::optional<int>> values;
};
sol::state lua;
auto table = devils_engine::bindings::to_lua(lua, settings{"world", UINT64_MAX, {1, {}}});
auto copy = devils_engine::bindings::from_lua<settings>(table);
auto text = devils_engine::bindings::lua_to_tavl(table);
auto parsed = devils_engine::bindings::tavl_to_lua(lua, text);
```

Conversions own their results. The Lua state must outlive returned sol objects. No Lua source is
generated or evaluated. Tables are read/written with raw C API operations; metatables are not
executed. C++ aggregates use `reflect` member names. Supported types:

- bool, signed/unsigned integers, finite floating point and enums by underlying integer;
- owned `std::string` (including binary bytes for C++/Lua);
- `std::optional`, `std::vector`, `std::array` and string-key `std::map`/`std::unordered_map`;
- nested reflect aggregates with required fields (optional members may be absent).

No numeric/string/bool coercion, silent integer truncation, unknown aggregate fields, sequence
holes or mixed sequence keys. Enums are not domain-validated; applications validate their values.
Pointers, engine handles, functions, coroutines and arbitrary userdata are outside this data API.
Cycles are rejected by the depth bound; repeated acyclic tables are copied as independent values.
There is no patch/update semantics: `from_lua<T>` creates a new `T`.

An explicit null sentinel preserves optional elements in sequences; Lua nil means an absent field.
Unsigned values up to `LUA_MAXINTEGER` are Lua integers. Larger uint64 values use immutable opaque
userdata, readable by `data_uint64`; this prevents rounding through double. They have no Lua
arithmetic operators. `push_data_uint64` constructs them; `push_data_null` constructs explicit null.
Do not replace these values with floating-point representations.

`conversion_limits` bounds one conversion: maximum depth (32), visited values (65536), cumulative
string bytes (1 MiB). Keys count toward string bytes. TAVL input length is also limited by
`string_bytes` before parsing. Limits are configurable; they do not replace the Lua allocator/host
heap budget. Lua allocator exhaustion follows Lua's own error mechanism. A host invoking a C++
converter directly must use a state whose allocator can satisfy the configured operation.
C++ conversion failures restore the Lua stack and report `utils::error` with the data path.

Lua environments created by `basic_functions` expose `base.parse_tavl(text)`,
`base.write_tavl(table)` and `base.null`, using the default conversion limits.

TAVL documents have an implicit record root. Fields use `=`; values may be scalars, objects,
arrays or tuples. Private weak-key metadata preserves table kinds, including empty containers,
without changing the user table metatable. Plain Lua tables use string keys for objects and a
dense 1-based integer sequence for arrays; an empty untagged table is an object. To create an empty
array/tuple explicitly, use `mark_data_table`. Nested strings are escaped by the TAVL serializer;
objects are written with sorted keys. Duplicate fields, custom operators, dates and DSL expressions
are refused. This API converts data, not mood/DS programs or arbitrary Lua state. TAVL text obeys
TAVL's UTF-8/control-character rules; binary strings remain supported by C++/Lua conversion.

# Critical native errors

`utils::error` throws `utils::critical_error` (a `std::runtime_error` subclass). Install
`install_critical_error_policy(L)` after opening Lua libraries and before copying sandbox globals
or loading user scripts. It installs the sol2 exception handler and pcall/xpcall implementations.
Ordinary Lua errors and ordinary native exceptions remain catchable. A critical native error is
latched per Lua registry, skips xpcall's user error handler and propagates through pcall/xpcall.
The latch retains the first original C++ exception.

At **every protected host call**, invoke `rethrow_critical_error(L)` before inspecting its result
or publishing output, even when that result is valid. `coroutine.resume` is another Lua protected
boundary and can return to script; the latch still prevents successful host completion. The host
check clears and rethrows the original exception, without unwinding C++ through Lua C frames.
States are independent. Opening base libraries again replaces pcall/xpcall: do not do so after
installing the policy. Raw third-party states/calls must explicitly follow this contract.

Tests: `bindings_data_test`, `originator_queue_lua_test`.
