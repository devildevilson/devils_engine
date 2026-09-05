#include "devils_engine/originator/device_form.h"

#include <format>

#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

std::string_view device_type_name(const field_base::values base) noexcept {
  switch (base) {
    case field_base::v: return "float";
    case field_base::ui: return "uint";
    case field_base::i: return "int";
    default: return {};
  }
}

namespace {
// Свёртка линейного индекса в координату картинки. Форма приезжает ОБЩЕЙ ШАПКОЙ, потому что она
// объявлена буфером (`extent`), и второго способа её назвать нет ни на одном из путей.
constexpr std::string_view to_coordinate = "ivec2(int(at % args.extent_x), int(at / args.extent_x))";

void emit_input(std::string& text, const size_t index, const uint32_t binding, const device_binding& shape) {
  const auto type = device_type_name(shape.base);
  if (shape.residence == device_residence::in_buffer) {
    text.append(std::format("layout(std430, binding = {}) readonly buffer in_{}_block {{ {} data[]; }} in_{}_raw;\n",
                            binding, index, type, index));
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
    text.append(std::format("void out_{}_set(uint at, {} value) {{ out_{}_raw.data[at] = value; }}\n",
                            index, type, index));
    text.append(std::format("uint out_{}_length() {{ return uint(out_{}_raw.data.length()); }}\n", index, index));
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
  text.append(std::format("uint out_{}_length() {{ return args.extent_x * args.extent_y; }}\n\n", index));
}
} // namespace

std::string build_device_shader(const std::span<const device_binding>& bindings,
                                const std::span<const device_param>& params,
                                const std::string_view& body,
                                const uint32_t group_size) {
  if (bindings.empty()) {
    utils::error{}("originator: a device form with no bindings computes nothing");
  }

  std::string text;
  text.append("#version 450\n\n");
  text.append(std::format("layout(local_size_x = {}) in;\n\n", group_size));

  // Шапка идёт ПЕРВОЙ, потому что аксессоры картинки сворачивают индекс по её полям: объявление,
  // которым пользуются, обязано стоять раньше того, кто им пользуется.
  text.append("layout(push_constant) uniform originator_call {\n");
  text.append("  uint count; uint begin; uint extent_x; uint extent_y;\n");
  for (const auto& param : params) {
    text.append(std::format("  float {};\n", param.shader_name()));
  }
  text.append("} args;\n\n");

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

  text.append("void main() {\n");
  // Свёртка числа групп в две оси: у Vulkan ГАРАНТИРОВАН только 65535 по X (§6.5), поэтому линейный
  // диспатч длиннее четырёх миллионов элементов законен не везде. Контекст раскладывает группы по
  // осям, а шейдер собирает индекс обратно — и оба конца этого соглашения живут в одном месте.
  text.append("  uint group = gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;\n");
  text.append("  uint local = group * gl_WorkGroupSize.x + gl_LocalInvocationID.x;\n");
  text.append("  if (local >= args.count) return;\n");
  text.append("  uint index = args.begin + local;\n");
  text.append(body);
  text.append("}\n");
  return text;
}

} // namespace originator
} // namespace devils_engine
