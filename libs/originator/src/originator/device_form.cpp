#include "devils_engine/originator/device_form.h"

#include <format>

#include "devils_engine/utils/core.h"

// Сборка текста вычислительного шейдера: преамбула случайности, привязки с типизированными
// аксессорами, свёртка индекса и тело.
//
// ОДИН СПОСОБ ОБРАТИТЬСЯ К ПОЛЮ НА ВСЕ ИНСТРУМЕНТЫ И ВСЕ ПЕРЕВОДЫ. Аксессор типизирован по
// ВЫВЕДЕННОМУ роду поля, поэтому `float[]` над сырым `uint32` здесь не выражается; у картинки тот же
// аксессор сворачивает линейный индекс в координату по форме из общей шапки.
//
// СВЁРТКА ЧИСЛА ГРУПП В ДВЕ ОСИ — часть соглашения, а не деталь: Vulkan гарантирует лишь 65535 групп
// по X, то есть 4.2 млн элементов при группе 64. Контекст раскладывает группы по осям, а шейдер
// собирает индекс обратно, и оба конца этого соглашения живут в одном месте.
//
// ХЕШ ВЫПИСАН ТЕКСТОМ, потому что у вычислительной программы нет `#include`: правка идёт в
// `utils/shared.h` первой, а тест сверяет оба пути побитово.

namespace devils_engine {
namespace originator {

translated_form translated_form::refused(std::string why) {
  translated_form result;
  result.refusal_ = std::move(why);
  return result;
}

translated_form::translated_form(authority,
                                 std::string body,
                                 std::vector<device_param> params,
                                 std::vector<uint32_t> filtered_inputs)
  : body_(std::move(body)), params_(std::move(params)), filtered_inputs_(std::move(filtered_inputs)) {}

bool translated_form::declared() const noexcept {
  return !body_.empty();
}

const std::string& translated_form::body() const noexcept {
  return body_;
}

const std::vector<device_param>& translated_form::params() const noexcept {
  return params_;
}

const std::vector<uint32_t>& translated_form::filtered_inputs() const noexcept {
  return filtered_inputs_;
}

const std::string& translated_form::refusal() const noexcept {
  return refusal_;
}

field_base::values device_storage_base(const field_base::values base) noexcept {
  switch (base) {
    case field_base::v:
    case field_base::sf:
    case field_base::c: return field_base::v;
    case field_base::ui:
    case field_base::us:
    case field_base::ub: return field_base::ui;
    case field_base::i:
    case field_base::is:
    case field_base::ib: return field_base::i;
    default: return field_base::count;
  }
}

device_value_range device_store_range(const field_base::values base) noexcept {
  // Числа те же, что в `store_component`, и повторены НАРОЧНО: две записи одного значения дали бы два
  // разных поля, а заметно это стало бы только на краю диапазона.
  switch (base) {
    case field_base::ui: return {0.0, 4294967295.0, true};
    case field_base::us: return {0.0, 65535.0, true};
    case field_base::ub: return {0.0, 255.0, true};
    case field_base::i: return {-2147483648.0, 2147483647.0, true};
    case field_base::is: return {-32768.0, 32767.0, true};
    case field_base::ib: return {-128.0, 127.0, true};
    case field_base::c: return {0.0, 1.0, true};
    // `v` не ограничен, а `sf` не зажимается и на хосте: `float_to_half` переполнение отдаёт
    // бесконечностью, и зажимать здесь значило бы разойтись с CPU.
    default: return {};
  }
}

bool exact_in_float(const field_base::values base) noexcept {
  switch (base) {
    case field_base::ub:
    case field_base::us:
    case field_base::ib:
    case field_base::is:
    case field_base::sf:
    case field_base::c: return true;
    default: return false;
  }
}

std::string_view device_type_name(const field_base::values base) noexcept {
  switch (base) {
    case field_base::v: return "float";
    case field_base::ui: return "uint";
    case field_base::i: return "int";
    default: return {};
  }
}

namespace {
// Хеш движка (`utils::shared::prng2`), выписанный текстом: вычислительная программа компилируется из
// ОДНОГО целого текста, `#include` у неё нет. Копия здесь — вынужденная, поэтому правка идёт в
// `utils/shared.h` первой, а тест сверяет оба пути побитово (`originator_translate_test`).
constexpr std::string_view random_preamble = R"(uint originator_prng2(uint s0, uint s1) {
  uint t = s1 ^ s0;
  uint n = ((s0 << 26) | (s0 >> 6)) ^ t ^ (t << 9);
  n = n * 0x9E3779BBu;
  return ((n << 5) | (n >> 27)) * 5u;
}

float originator_normalize(uint state) {
  return uintBitsToFloat((0x7fu << 23) | (state >> 9)) - 1.0;
}

// A value in [0, 1) for element `at` at call site `site`. The offsets keep the arguments away from
// zero, which is the one input this hash maps to itself.
float originator_chance(uint at, uint site) {
  uint state = originator_prng2(args.seed + 0x9E3779B9u, at + 0x85EBCA6Bu);
  return originator_normalize(originator_prng2(state + 0xC2B2AE35u, site + 0x27D4EB2Fu));
}

// Hash of the VALUE, not of the element: the same argument gives the same result in every invocation
// and in every run, which is what makes it usable as stable per-value noise.
float originator_mix1(float v) {
  return originator_normalize(originator_prng2(floatBitsToUint(v) + 0x9E3779B9u, 0x85EBCA6Bu));
}

float originator_mix2(float a, float b) {
  return originator_normalize(originator_prng2(floatBitsToUint(a) + 0x9E3779B9u, floatBitsToUint(b) + 0x85EBCA6Bu));
}

)";

// Свёртка линейного индекса в координату картинки. Форма приезжает ОБЩЕЙ ШАПКОЙ, потому что она
// объявлена буфером (`extent`), и второго способа её назвать нет ни на одном из путей.
constexpr std::string_view to_coordinate = "ivec2(int(at % args.extent_x), int(at / args.extent_x))";

void emit_input(std::string& text, const size_t index, const uint32_t binding, const device_binding& shape) {
  const auto type = device_type_name(shape.base);
  if (shape.residence == device_residence::in_buffer) {
    text.append(std::format("layout(std430, binding = {}) readonly buffer in_{}_block {{ {} data[]; }} in_{}_raw;\n",
                            binding, index, type, index));
    // МНОГОКОМПОНЕНТНОЕ ПОЛЕ читается тем же буфером: в раскладке `soa` компоненты лежат подряд
    // внутри элемента, поэтому нужен только аксессор, берущий компоненту, и длина, считающая
    // ЭЛЕМЕНТЫ, а не числа. `in_i_at(at)` при этом остаётся нулевой компонентой — у однокомпонентного
    // поля это то же самое, и тело, написанное для одного, не ломается на другом.
    if (shape.components > 1) {
      text.append(std::format("{} in_{}_at(uint at, uint component) {{ return in_{}_raw.data[at * {}u + component]; }}\n",
                              type, index, index, shape.components));
      text.append(std::format("{} in_{}_at(uint at) {{ return in_{}_raw.data[at * {}u]; }}\n",
                              type, index, index, shape.components));
      text.append(std::format("uint in_{}_length() {{ return uint(in_{}_raw.data.length()) / {}u; }}\n\n",
                              index, index, shape.components));
      return;
    }
    text.append(std::format("{} in_{}_at(uint at) {{ return in_{}_raw.data[at]; }}\n", type, index, index));
    text.append(std::format("uint in_{}_length() {{ return uint(in_{}_raw.data.length()); }}\n\n", index, index));
    return;
  }

  // КАРТИНКА. Фильтруемый вход приходит с сэмплером, потому что фильтр — единственное, ради чего род
  // вообще сменился; остальные читают тот же образ storage-привязкой.
  if (shape.access == device_access::filtered) {
    text.append(std::format("layout(binding = {}) uniform sampler2D in_{}_raw;\n", binding, index));
    // Индексного доступа у фильтруемого входа НЕТ намеренно: его форма не обязана совпадать с формой
    // вызова (грубое поле в мелкий чанк — это §6.3), а свернуть индекс можно только по своей форме.
    // Дать здесь `in_i_at` значило бы дать доступ, который молча читает не тот тексель.
    text.append(std::format("float in_{}_sample(vec2 uv) {{ return texture(in_{}_raw, uv).r; }}\n\n", index, index));
    return;
  }

  text.append(std::format("layout(r32f, binding = {}) readonly uniform image2D in_{}_raw;\n", binding, index));
  text.append(std::format("float in_{}_at(uint at) {{ return imageLoad(in_{}_raw, {}).r; }}\n",
                          index, index, to_coordinate));
  text.append(std::format("uint in_{}_length() {{ return args.extent_x * args.extent_y; }}\n\n", index));
}

void emit_output(std::string& text, const size_t index, const uint32_t binding, const device_binding& shape) {
  const auto type = device_type_name(shape.base);
  if (shape.residence == device_residence::in_buffer) {
    // Без `writeonly`: накопитель обязан читать то, к чему прибавляет, а квалификатор, который то
    // есть, то нет, — лишний повод разъехаться.
    text.append(std::format("layout(std430, binding = {}) buffer out_{}_block {{ {} data[]; }} out_{}_raw;\n",
                            binding, index, type, index));
    text.append(std::format("uint out_{}_components() {{ return {}u; }}\n", index, shape.components));
    text.append(std::format("uint out_{}_length() {{ return uint(out_{}_raw.data.length()) / {}u; }}\n",
                            index, index, shape.components));
    // Компонента адресуется отдельно, а форма без неё пишет НУЛЕВУЮ: у однокомпонентного поля это
    // одно и то же, поэтому тело, написанное для одного, не ломается на другом.
    text.append(std::format("void out_{}_set(uint at, uint component, {} value) {{ out_{}_raw.data[at * {}u + component] = value; }}\n",
                            index, type, index, shape.components));
    text.append(std::format("void out_{}_set(uint at, {} value) {{ out_{}_raw.data[at * {}u] = value; }}\n",
                            index, type, index, shape.components));
    if (shape.converting && shape.base != field_base::v) {
      // Тело, написанное против `float`, над целым полем: ПРЕОБРАЗОВАНИЕ значения, а не переклад
      // битов, — то же самое, что делает аксессор на хосте.
      text.append(std::format("void out_{}_set(uint at, uint component, float value) {{ out_{}_set(at, component, {}(value)); }}\n",
                              index, index, type));
      text.append(std::format("void out_{}_set(uint at, float value) {{ out_{}_set(at, 0u, {}(value)); }}\n",
                              index, index, type));
    }
    if (shape.accumulates) {
      text.append(std::format("void out_{}_add(uint at, {} value) {{ atomicAdd(out_{}_raw.data[at], value); }}\n",
                              index, type, index));
    }
    text.append("\n");
    return;
  }

  text.append(std::format("layout(r32f, binding = {}) uniform image2D out_{}_raw;\n", binding, index));
  text.append(std::format("void out_{}_set(uint at, float value) {{ imageStore(out_{}_raw, {}, vec4(value, 0.0, 0.0, 0.0)); }}\n",
                          index, index, to_coordinate));
  text.append(std::format("void out_{}_set(uint at, uint component, float value) {{ out_{}_set(at, value); }}\n",
                          index, index));
  text.append(std::format("uint out_{}_components() {{ return 1u; }}\n", index));
  text.append(std::format("uint out_{}_length() {{ return args.extent_x * args.extent_y; }}\n\n", index));
}
} // namespace

std::string build_device_shader(const std::span<const device_binding>& bindings,
                                const std::span<const device_param>& params,
                                const std::string_view& body,
                                const uint32_t group_size,
                                const std::string_view& prelude,
                                const bool whole_group) {
  if (bindings.empty()) {
    utils::error{}("originator: a device form with no bindings computes nothing");
  }
  for (size_t i = 0; i < bindings.size(); ++i) {
    if (bindings[i].components != 1 && bindings[i].residence == device_residence::in_image) {
      utils::error{}("originator: binding {} is a multi-component image, and the queue keeps images single-channel", i);
    }
  }

  // ПРЕДЕЛ PUSH-КОНСТАНТЫ ПРОВЕРЯЕТСЯ ЗДЕСЬ, потому что здесь она и объявляется, — один раз на все
  // тела и все переводы. Превышение это ошибка объявления, а не свойство машины: `device_push_limit`
  // выбран так, что помещается везде.
  if (params.size() > device_param_limit) {
    utils::error{}("originator: a device form declares {} parameters, and {} bytes of push constant hold "
                   "a header plus {}",
                   params.size(), device_push_limit, device_param_limit);
  }

  std::string text;
  text.append("#version 450\n\n");
  text.append(std::format("layout(local_size_x = {}) in;\n\n", group_size));

  // Шапка идёт ПЕРВОЙ, потому что аксессоры картинки сворачивают индекс по её полям: объявление,
  // которым пользуются, обязано стоять раньше того, кто им пользуется.
  text.append("layout(push_constant) uniform originator_call {\n");
  text.append("  uint count; uint begin; uint extent_x; uint extent_y; uint seed;\n");
  text.append("  uint raw_seed_lo; uint raw_seed_hi;\n");
  for (const auto& param : params) {
    text.append(std::format("  float {};\n", param.shader_name()));
  }
  text.append("} args;\n\n");

  // СЛУЧАЙНОСТЬ ОДНА НА ВСЕ ТЕЛА, и объявлена она здесь по той же причине, по какой здесь собираются
  // привязки: второй способ получить случайное число разъехался бы с первым молча. Зерно приходит
  // общей шапкой, поэтому значение — функция от (зерно вызова, элемент, место вызова) и больше ни от
  // чего: разбиение работы по инвокациям на него не влияет.
  text.append(random_preamble);

  // СКОЛЬКО ПРИВЯЗОК У ЭТОГО ВЫЗОВА — доступно телу как определение препроцессора. Нужно там, где у
  // инструмента есть НЕОБЯЗАТЕЛЬНЫЙ выход: непривязанного выхода в шейдере нет вовсе, поэтому тело
  // обязано уметь спросить, а не полагаться на то, что аксессор объявлен.
  size_t declared_inputs = 0;
  size_t declared_outputs = 0;
  for (const auto& shape : bindings) {
    (shape.writable ? declared_outputs : declared_inputs) += 1;
  }
  text.append(std::format("#define ORIGINATOR_INPUTS {}\n", declared_inputs));
  text.append(std::format("#define ORIGINATOR_OUTPUTS {}\n", declared_outputs));

  // СКОЛЬКО КОМПОНЕНТ У ПРИВЯЗКИ — тоже определением препроцессора, и по той же причине: тело,
  // работающее и над плоскостью, и над объёмом, обязано СПРОСИТЬ, а не полагаться на то, что аксессор
  // с компонентой вообще объявлен (у однокомпонентного поля его нет).
  {
    size_t counted_inputs = 0;
    size_t counted_outputs = 0;
    for (const auto& shape : bindings) {
      const auto slot = shape.writable ? counted_outputs++ : counted_inputs++;
      text.append(std::format("#define ORIGINATOR_{}_{}_COMPONENTS {}\n", shape.writable ? "OUT" : "IN", slot,
                              shape.components));
    }
  }
  text.push_back('\n');

  size_t inputs = 0;
  size_t outputs = 0;
  for (size_t i = 0; i < bindings.size(); ++i) {
    const auto& shape = bindings[i];
    if (device_type_name(shape.base).empty()) {
      utils::error{}("originator: binding {} has no 32-bit kind, so it has no shader type", i);
    }
    if (shape.writable) {
      emit_output(text, outputs++, uint32_t(i), shape);
    } else {
      emit_input(text, inputs++, uint32_t(i), shape);
    }
  }

  // ФУНКЦИИ ТЕЛА идут после привязок: преамбула вправе пользоваться и аксессорами, и push-константой,
  // а объявить их внутри `main` нельзя.
  if (!prelude.empty()) {
    text.append(prelude);
    text.push_back('\n');
  }

  text.append("void main() {\n");
  // Свёртка числа групп в две оси: у Vulkan ГАРАНТИРОВАН только 65535 по X (§6.5), поэтому линейный
  // диспатч длиннее четырёх миллионов элементов законен не везде. Контекст раскладывает группы по
  // осям, а шейдер собирает индекс обратно — и оба конца этого соглашения живут в одном месте.
  text.append("  uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;\n");
  text.append("  uint local = group * gl_WorkGroupSize.x + gl_LocalInvocationID.x;\n");
  if (whole_group) {
    // ОХРАННИК ПЕРЕЕЗЖАЕТ В ТЕЛО: до барьера обязаны дойти ВСЕ инвокации группы, а ранний возврат
    // это ровно то, что им мешает. Индекс у неактивной инвокации прижат к началу диапазона, чтобы
    // случайное чтение осталось в границах буфера, а не ушло за него.
    // Признак зовётся `in_range`, а не `active`: `active` в GLSL — ЗАРЕЗЕРВИРОВАННОЕ слово, и ловится
    // это только компилятором. Ровно тот случай, ради которого проверку и отдали glslc.
    text.append("  bool in_range = local < args.count;\n");
    text.append("  uint index = args.begin + (in_range ? local : 0u);\n");
  } else {
    text.append("  if (local >= args.count) return;\n");
    text.append("  uint index = args.begin + local;\n");
  }
  text.append(body);
  text.append("}\n");
  return text;
}

} // namespace originator
} // namespace devils_engine
