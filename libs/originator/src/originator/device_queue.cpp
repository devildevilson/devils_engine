#include "devils_engine/originator/device_queue.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include "devils_engine/originator/device_form.h"
#include "devils_engine/utils/core.h"

// Устройственный план очереди: originator СОСТАВЛЯЕТ pipeline из того же объявления, которое
// проверяет `check_queue`.
//
// ВСЁ ВЫВОДИТСЯ ИЗ ОЧЕРЕДИ, второго планировщика не появляется:
//   ПЕРЕДАЧА — из границ (загружается то, что вызов читает и до него никто не писал; выгружается
//   ровно `output`, а `resident` остаётся на устройстве);
//   БАРЬЕРЫ — из того же вопроса, что и проверка мёртвой работы;
//   РОД РЕСУРСА — из того, читает ли поле хоть один вызов ФИЛЬТРОМ (тогда картинка, иначе буфер);
//   ФОРМА вызова объявляется тем, кто его собрал: инструментом или переводом `ds`.
//
// ТЕКСТ ШЕЙДЕРА СОБИРАЕТСЯ ЗДЕСЬ, потому что только здесь известен выведенный род каждого поля; кэш
// программ контекста ключуется по этому тексту, поэтому одинаковые вызовы компилируются один раз.
//
// ВСЁ УХОДИТ ОДНОЙ ОТПРАВКОЙ. Пошаговая отправка мерилась намеренно и оказалась в 2.4 раза дороже:
// то, что выглядело ценой ПЕРЕДАЧИ, было в основном ценой ОТПРАВКИ.

namespace devils_engine {
namespace originator {

namespace {
// Поле, которое очередь трогает, вместе со всем, что о нём ВЫВОДИТСЯ.
struct plan_field {
  field_ref field{};
  bool read_before_written = false;
  bool written = false;
  // Кто-то читает это поле ФИЛЬТРОМ. Единственный признак, из-за которого поле меняет род: всё
  // остальное буфер делает лучше (§6.3).
  bool sampled = false;
};

std::string field_name_of(const field_ref& field) {
  return std::format("{}.{}", field.buffer_name(), field.field_name());
}

// Род поля, у которого на устройстве есть представление. Узкие роды существуют ради КОМПАКТНОГО
// хранения на CPU, а считать в них никто и не собирался (§3.2), поэтому ограничение почти не задевает
// того, для чего они заведены.
bool device_field_kind(const field_type type) noexcept {
  // Число КОМПОНЕНТ ограничения не ставит: в раскладке `soa` они лежат подряд внутри элемента,
  // поэтому поле читается тем же буфером, только аксессором с компонентой. Ограничение — на РОД: у
  // узких родов нет типизированного буфера шейдера, и заведены они ради компактного хранения на
  // хосте, а не ради счёта (§3.2).
  return type.components >= 1 && type.components <= max_field_components &&
         (type.base == field_base::v || type.base == field_base::ui || type.base == field_base::i);
}

std::string_view refuse_field(const field_ref&) noexcept {
  return "the queue takes the 32-bit kinds v, ui and i on a device; the narrow kinds exist for compact "
         "storage on the host, and computing in them was never the point";
}

// ОТКУДА У ВЫЗОВА ТЕЛО. Ровно два источника, и оба контролируемые: у нативного инструмента оно
// написано заранее и лежит в библиотеке, у чужого тела — это перевод программы `devils_script`.
const std::string& body_of(const queue_call& call) noexcept {
  return call.tool != nullptr ? call.tool->device_body : call.device.body();
}

const std::vector<device_param>& params_of(const queue_call& call) noexcept {
  return call.tool != nullptr ? call.tool->device_params : call.device.params();
}

const std::vector<uint32_t>& filtered_of(const queue_call& call) noexcept {
  return call.tool != nullptr ? call.tool->device_filtered_inputs : call.device.filtered_inputs();
}

bool order_free_of(const queue_call& call) noexcept {
  return call.tool != nullptr ? call.tool->order_free_writes : call.order_free_writes;
}

// ФОРМА ВЫЗОВА. Берётся у того, что диапазон вызова ИНДЕКСИРУЕТ, — то же правило, по которому
// `check_dispatch` решает, к чему диапазон относится: у scatter это входы, у остальных выходы.
// Второго ответа на этот вопрос в библиотеке быть не должно.
const field_ref& range_anchor(const queue_call& call) noexcept {
  if (call.shape == aperture::scatter && !call.inputs.empty()) {
    return call.inputs.front();
  }
  return call.outputs.front();
}

buffer_extent call_extent(const queue_call& call) noexcept {
  const auto& anchor = range_anchor(call);
  const auto declared = anchor.extent();
  if (declared.declared() && declared.axes() >= 2) {
    return declared;
  }
  return buffer_extent{anchor.count(), 1, 0};
}

// РОД ПОЛЯ ВЫВОДИТСЯ, а форма объявляется (§6.2). Здесь и происходит вывод: поле, которое хоть один
// вызов читает фильтром, обязано стать картинкой, остальные остаются буферами.
//
// Отказы возвращаются текстом, а не бросаются: «на устройство не переносится» — законный ответ, по
// которому вызывающий уходит на CPU, а не ошибка конфига.
std::string derive_fields(const computation_queue& queue, std::vector<plan_field>& fields) {
  fields.clear();

  const auto slot_of = [&](const field_ref& field) -> size_t {
    for (size_t i = 0; i < fields.size(); ++i) {
      if (fields[i].field.same_field_as(field)) return i;
    }
    fields.push_back(plan_field{field, false, false, false});
    return fields.size() - 1;
  };

  for (size_t i = 0; i < queue.calls.size(); ++i) {
    const auto& call = queue.calls[i];
    for (const auto& binding : call.inputs) {
      const auto index = slot_of(binding);
      if (!fields[index].written) {
        fields[index].read_before_written = true;
      }
    }
    if (call.indirect()) {
      const auto index = slot_of(call.count_from);
      if (!fields[index].written) {
        fields[index].read_before_written = true;
      }
    }
    for (const auto& binding : call.outputs) {
      const auto index = slot_of(binding);
      // НАКОПИТЕЛЬ ЧИТАЕТ ТО, ВО ЧТО ПИШЕТ — тот же вывод, что у проверки мёртвой работы, и здесь он
      // решает, приедет ли поле на устройство. Без него буфер гистограммы достался бы шейдеру с
      // мусором VMA, и сводка вышла бы правдоподобной и неверной: атомик только прибавляет.
      if (order_free_of(call) && !fields[index].written) {
        fields[index].read_before_written = true;
      }
      fields[index].written = true;
    }

    for (const auto position : filtered_of(call)) {
      if (position >= call.inputs.size()) {
        return std::format("step '{}': queue element {} ('{}') declares input {} filtered, but it binds only {}",
                           queue.name, i + 1, call.label, position, call.inputs.size());
      }
      fields[slot_of(call.inputs[position])].sampled = true;
    }
  }

  for (const auto& entry : fields) {
    if (!entry.sampled) continue;

    // Фильтр между ЦЕЛЫМИ не имеет смысла: среднее двух номеров области — не номер области. Поэтому
    // картинкой становится только плавающее поле, и формат у неё один — `r32f`.
    if (entry.field.type().base != field_base::v || entry.field.type().components != 1) {
      return std::format("step '{}': '{}' is read filtered, but it is not a single floating value — a filter "
                         "averages between elements, and the average of two integer labels is not a label",
                         queue.name, field_name_of(entry.field));
    }

    // ФОРМА ОБЪЯВЛЯЕТСЯ. Читать между элементами можно только зная, между какими: без `extent` у
    // буфера нет ни строки, ни столбца, и координаты выборки взять неоткуда.
    const auto extent = entry.field.extent();
    if (!extent.declared() || extent.axes() < 2) {
      return std::format("step '{}': '{}' is read filtered, but its buffer declares no 2D extent — reading "
                         "BETWEEN elements needs to know between which, and that is what 'extent' says",
                         queue.name, field_name_of(entry.field));
    }
    if (extent.x * extent.y != entry.field.count()) {
      return std::format("step '{}': '{}' declares extent {}x{} but holds {} elements",
                         queue.name, field_name_of(entry.field), extent.x, extent.y, entry.field.count());
    }
  }

  return {};
}

device_residence::values residence_of(const std::vector<plan_field>& fields, const field_ref& binding) noexcept {
  for (const auto& entry : fields) {
    if (entry.field.same_field_as(binding)) {
      return entry.sampled ? device_residence::in_image : device_residence::in_buffer;
    }
  }
  return device_residence::in_buffer;
}
} // namespace

struct device_queue::field_slot {
  field_ref field{};
  device_residence::values residence = device_residence::in_buffer;
  painter::compute_context::buffer_id device_buffer = painter::compute_context::invalid_id;
  painter::compute_context::image_id device_image = painter::compute_context::invalid_id;
  painter::compute_context::buffer_id staging = painter::compute_context::invalid_id;
  size_t byte_size = 0;
};

struct device_queue::call_plan {
  painter::compute_context::program_id program = painter::compute_context::invalid_id;
  painter::compute_context::binding_set_id set = painter::compute_context::invalid_id;
  std::vector<std::byte> push;
  size_t element_count = 0;
  bool barrier_before = false;
};

device_check check_device_queue(const computation_queue& queue) {
  device_check result;

  const auto fail = [&](std::string message) {
    result.allowed = false;
    result.message = std::move(message);
    return result;
  };

  // Сначала те же правила, что у пути на CPU: устройственный план не заводит второго набора проверок,
  // он добавляет только то, чего на CPU не бывает.
  const auto base = check_queue(queue);
  if (!base.allowed) {
    return fail(base.message);
  }

  std::vector<plan_field> fields;
  const auto derived = derive_fields(queue, fields);
  if (!derived.empty()) {
    return fail(derived);
  }

  for (size_t i = 0; i < queue.calls.size(); ++i) {
    const auto& call = queue.calls[i];
    const size_t position = i + 1;

    const bool native = call.tool != nullptr;
    if (body_of(call).empty()) {
      const auto reason = native
                            ? std::string("the tool declares no device form")
                            : (call.device.refusal().empty()
                                 ? std::string("the call carries no device form; a foreign body reaches a device "
                                               "only as a translated devils_script program")
                                 : call.device.refusal());
      return fail(std::format("step '{}': queue element {} ('{}') does not run on a device: {}",
                              queue.name, position, call.label, reason));
    }

    // КОСВЕННЫЙ ДИАПАЗОН на устройстве — это `dispatchIndirect`, а его здесь нет. Прежде такой вызов
    // молча получал `range_count() == 0` и не делал НИЧЕГО: диапазон со счётчиком не задан числом.
    // Проход, не сделавший ничего, по результату отличим от сделавшего только тем, что поле осталось
    // прежним, — а оно и так бывает прежним.
    if (call.indirect()) {
      return fail(std::format("step '{}': queue element {} ('{}') counts its elements from '{}.{}', and a device "
                              "plan has no indirect dispatch yet — its range would silently be zero",
                              queue.name, position, call.label, call.count_from.buffer_name(),
                              call.count_from.field_name()));
    }

    const auto extent = call_extent(call);

    const auto inspect = [&](const field_ref& binding, const bool writes) -> std::string {
      if (!device_field_kind(binding.type())) {
        return std::format("step '{}': queue element {} ('{}') {} '{}', and {}", queue.name, position,
                           call.label, writes ? "writes" : "reads", field_name_of(binding),
                           refuse_field(binding));
      }

      // РАСКЛАДКА ОБЯЗАНА БЫТЬ `soa`, и это не придирка. Поле `aos` лежит с шагом элемента, то есть
      // непрерывным блоком его не передать, а страйд на устройстве стоит линейно по шагу (измерено:
      // шаг 4 — вдвое-втрое, шаг 8 — вчетверо-впятеро).
      if (binding.source == nullptr || binding.source->layout().storage != storage_kind::soa) {
        return std::format("step '{}': buffer '{}' is laid out as aos, and a device queue needs soa — the field "
                           "must lie contiguously to be transferred, and a stride costs linearly on a device",
                           queue.name, binding.buffer_name());
      }

      // ТЕЛО НАТИВНОГО ИНСТРУМЕНТА ПИШЕТСЯ ПРОТИВ `float`, если инструмент не объявил обратного: его
      // писали один раз на все будущие привязки. Прежде текст объявлял `float data[]` у ЛЮБОГО поля,
      // и вызов над целым полем молча читал биты — теперь это отказ.
      if (native && !call.tool->device_integer_ready && binding.type().base != field_base::v) {
        return std::format("step '{}': queue element {} ('{}') binds '{}' of kind '{}', but the device body of a "
                           "native tool is written against float — it is written once for every future binding, "
                           "so it cannot know the kind. Reading that field as float would take its BITS",
                           queue.name, position, call.label, field_name_of(binding),
                           to_string(binding.type().base));
      }

      // Индексный доступ к картинке сворачивает индекс по форме ВЫЗОВА, поэтому картинка другой формы
      // так не читается. Читать её можно выборкой — и тогда вызов объявляет вход фильтруемым.
      if (residence_of(fields, binding) == device_residence::in_image) {
        const auto own = binding.extent();
        if (own.x != extent.x || own.y != extent.y) {
          return std::format("step '{}': queue element {} ('{}') addresses '{}' by index, but that field is an "
                             "image of {}x{} while the call has shape {}x{} — an index folds by the CALL's shape, "
                             "so it would read the wrong texel; read it filtered instead",
                             queue.name, position, call.label, field_name_of(binding), own.x, own.y,
                             extent.x, extent.y);
        }
      }
      return {};
    };

    const auto& filtered = filtered_of(call);
    for (size_t k = 0; k < call.inputs.size(); ++k) {
      const bool by_filter = std::find(filtered.begin(), filtered.end(), uint32_t(k)) != filtered.end();
      const auto& binding = call.inputs[k];
      if (by_filter) {
        // У фильтруемого входа своя форма и своя система координат: сверять её с формой вызова не
        // надо, ради этого выборка и существует (грубое поле в мелкий чанк, §6.3).
        if (!device_field_kind(binding.type())) {
          return fail(std::format("step '{}': queue element {} ('{}') reads '{}' filtered, and {}", queue.name,
                                  position, call.label, field_name_of(binding), refuse_field(binding)));
        }
        continue;
      }
      const auto message = inspect(binding, false);
      if (!message.empty()) return fail(message);
    }
    for (const auto& binding : call.outputs) {
      const auto message = inspect(binding, true);
      if (!message.empty()) return fail(message);
    }

    // НАКОПИТЕЛЬ. Атомарное сложение есть у целого буфера и только у него: у плавающего порядок
    // значим (§4.3), а у картинки атомиков нет вовсе без расширения.
    if (order_free_of(call)) {
      for (const auto& binding : call.outputs) {
        if (residence_of(fields, binding) == device_residence::in_image) {
          return fail(std::format("step '{}': queue element {} ('{}') accumulates into '{}', but that field is an "
                                  "image, and an image has no atomic accumulation",
                                  queue.name, position, call.label, field_name_of(binding)));
        }
        if (binding.type().base == field_base::v) {
          return fail(std::format("step '{}': queue element {} ('{}') declares its writes order-free but "
                                  "accumulates into the floating field '{}' — the order of floating addition IS "
                                  "significant, and declaring otherwise would simply be untrue",
                                  queue.name, position, call.label, field_name_of(binding)));
        }
      }
    }
  }

  return result;
}

device_queue::device_queue(painter::compute_context& context, const computation_queue& queue)
  : context_(&context), queue_(queue) {
  const auto check = check_device_queue(queue_);
  if (!check.allowed) {
    utils::error{}("originator {}", check.message);
  }

  // 1. ЧТО ТРОГАЕТ ОЧЕРЕДЬ и ЧЕМ оно на устройстве является. Обход тот же, которым `check_queue`
  //    ищет мёртвую работу: поле, которое вызов читает и которого до него никто не писал, — это ВХОД,
  //    а что уезжает наружу, сказано в `output`. Оба списка вычисляются, а не объявляются.
  std::vector<plan_field> fields;
  const auto derived = derive_fields(queue_, fields);
  if (!derived.empty()) {
    utils::error{}("originator {}", derived);
  }

  const auto slot_of = [&](const field_ref& field) -> size_t {
    for (size_t i = 0; i < slots_.size(); ++i) {
      if (slots_[i].field.same_field_as(field)) return i;
    }
    utils::error{}("originator step '{}': '{}' was never derived into the plan", queue_.name,
                   field_name_of(field));
    return 0;
  };

  slots_.reserve(fields.size());
  for (const auto& entry : fields) {
    field_slot slot;
    slot.field = entry.field;
    slot.byte_size = entry.field.count() * entry.field.type().byte_size();
    slot.residence = entry.sampled ? device_residence::in_image : device_residence::in_buffer;

    if (slot.residence == device_residence::in_image) {
      const auto extent = entry.field.extent();
      const auto side = context_->limits().max_image_2d;
      if (extent.x > side || extent.y > side) {
        utils::error{}("originator step '{}': '{}' would be a {}x{} image, and '{}' takes at most {} per axis",
                       queue_.name, field_name_of(entry.field), extent.x, extent.y, context_->device_name(), side);
      }
      slot.device_image = context_->create_image(uint32_t(extent.x), uint32_t(extent.y),
                                                 painter::compute_context::image_format::r32f);
    } else {
      slot.device_buffer = context_->create_buffer(slot.byte_size, false);
    }
    slots_.push_back(slot);
  }

  for (size_t i = 0; i < fields.size(); ++i) {
    if (!fields[i].read_before_written) continue;
    slots_[i].staging = context_->create_buffer(slots_[i].byte_size, true);
    upload_slots_.push_back(i);
    uploaded_.push_back(field_name_of(fields[i].field));
    upload_bytes_ += slots_[i].byte_size;
  }

  for (const auto& binding : queue_.output) {
    const auto index = slot_of(binding);
    if (std::find(download_slots_.begin(), download_slots_.end(), index) != download_slots_.end()) {
      continue;
    }
    if (slots_[index].staging == painter::compute_context::invalid_id) {
      slots_[index].staging = context_->create_buffer(slots_[index].byte_size, true);
    }
    download_slots_.push_back(index);
    downloaded_.push_back(field_name_of(binding));
    download_bytes_ += slots_[index].byte_size;
  }

  // 2. ПРОГРАММА И ПРИВЯЗКИ НА ВЫЗОВ. Набор дескрипторов принадлежит вызову, а не программе: два
  //    `remap` подряд — одна программа над разными полями, и общий набор дал бы обоим ПОСЛЕДНИЕ
  //    привязки, потому что дескрипторы вступают в силу при отправке.
  plans_.reserve(queue_.calls.size());
  for (size_t i = 0; i < queue_.calls.size(); ++i) {
    const auto& call = queue_.calls[i];
    const auto& params = params_of(call);
    const auto& filtered = filtered_of(call);
    const bool accumulates = order_free_of(call);

    std::vector<device_binding> shape;
    std::vector<painter::compute_context::binding_kind> kinds;
    std::vector<painter::compute_context::bound_resource> resources;
    shape.reserve(call.inputs.size() + call.outputs.size());
    kinds.reserve(shape.capacity());
    resources.reserve(shape.capacity());

    const auto bind = [&](const field_ref& binding, const bool writes, const bool by_filter) {
      const auto& slot = slots_[slot_of(binding)];
      device_binding declared;
      declared.base = binding.type().base;
      declared.components = binding.type().components;
      declared.residence = slot.residence;
      declared.access = by_filter ? device_access::filtered : device_access::plain;
      declared.writable = writes;
      declared.accumulates = writes && accumulates && slot.residence == device_residence::in_buffer;
      declared.converting = call.tool != nullptr && call.tool->device_integer_ready;
      shape.push_back(declared);

      if (slot.residence == device_residence::in_image) {
        kinds.push_back(by_filter ? painter::compute_context::binding_kind::sampled_image
                                  : painter::compute_context::binding_kind::storage_image);
        resources.push_back(painter::compute_context::bound_resource::of_image(slot.device_image));
        return;
      }
      kinds.push_back(painter::compute_context::binding_kind::storage_buffer);
      resources.push_back(painter::compute_context::bound_resource::of_buffer(slot.device_buffer));
    };

    for (size_t k = 0; k < call.inputs.size(); ++k) {
      bind(call.inputs[k], false, std::find(filtered.begin(), filtered.end(), uint32_t(k)) != filtered.end());
    }
    for (const auto& binding : call.outputs) {
      bind(binding, true, false);
    }

    // 3. ТЕКСТ СОБИРАЕТСЯ ЗДЕСЬ, потому что только здесь известен выведенный род каждого поля. Тот же
    //    текст на тех же привязках даёт ту же программу — кэш контекста ключуется по нему.
    const auto source = build_device_shader(shape, params, body_of(call));
    const uint32_t push_size = uint32_t(sizeof(device_call_header) + params.size() * sizeof(float));

    call_plan plan;
    plan.program = context_->create_program(call.label, source, kinds, push_size);
    plan.set = context_->create_binding_set(plan.program);
    context_->update_binding_set(plan.set, resources);

    // 4. PUSH-КОНСТАНТА по ОБЩЕМУ соглашению: шапка, затем объявленные параметры по одному float в
    //    объявленном порядке. Один способ выложить байты на все инструменты и все переводы — два
    //    способа однажды разъехались бы, а шейдер прочитал бы чужие числа и не пожаловался.
    plan.push.resize(push_size);
    write_push(call, plan);

    // 5. БАРЬЕР ВЫВОДИТСЯ. Тот же вопрос, что у проверки мёртвой работы: читает ли этот проход то,
    //    что записал какой-то предыдущий. Есть зависимость — есть барьер, нет — нет, и это не
    //    оптимизация, а следствие анализа, который уже был нужен по другой причине.
    for (size_t earlier = 0; earlier < i && !plan.barrier_before; ++earlier) {
      for (const auto& written : queue_.calls[earlier].outputs) {
        if (!call.reads(written) && !call.writes(written)) continue;
        plan.barrier_before = true;
        break;
      }
    }
    if (plan.barrier_before) {
      ++barriers_;
    }

    plans_.push_back(std::move(plan));
  }
}

device_queue::~device_queue() noexcept = default;

void device_queue::write_push(const queue_call& call, call_plan& plan) const {
  const auto& params = params_of(call);
  const auto extent = call_extent(call);

  device_call_header header;
  header.count = uint32_t(call.range_count());
  header.begin = uint32_t(call.range_begin);
  header.extent_x = uint32_t(extent.x);
  header.extent_y = uint32_t(extent.y == 0 ? 1 : extent.y);
  // Зерно ВЫЗОВА, а не шага: у вызова оно уже посчитано (`hash(зерно пайплайна, имя шага)` либо
  // названное в lua), и устройство обязано взять то же самое число, что взял бы CPU.
  header.seed = fold_seed(call.seed);
  std::memcpy(plan.push.data(), &header, sizeof(header));

  for (size_t p = 0; p < params.size(); ++p) {
    // Значение по умолчанию берётся у САМОГО инструмента через его же чтение параметров: числа,
    // которых в вызове нет, обязаны совпасть с тем, что подставил бы CPU.
    const float value = float(call.params.number(params[p].name, params[p].fallback));
    std::memcpy(plan.push.data() + sizeof(header) + p * sizeof(float), &value, sizeof(value));
  }

  plan.element_count = call.range_count();
}

device_report device_queue::run(const computation_queue& current) {
  if (current.calls.size() != plans_.size()) {
    utils::error{}("originator step '{}': the plan holds {} calls and was handed {}", queue_.name,
                   plans_.size(), current.calls.size());
  }

  for (size_t i = 0; i < plans_.size(); ++i) {
    const auto& call = current.calls[i];
    const auto& known = queue_.calls[i];
    if (call.tool != known.tool || call.label != known.label || call.inputs.size() != known.inputs.size() ||
        call.outputs.size() != known.outputs.size() || call.range_begin != known.range_begin ||
        call.range_end != known.range_end) {
      utils::error{}("originator step '{}': queue element {} no longer matches the plan built for it",
                     queue_.name, i + 1);
    }
    write_push(call, plans_[i]);
  }
  return execute(true);
}

const std::vector<std::string>& device_queue::uploaded_fields() const noexcept {
  return uploaded_;
}

const std::vector<std::string>& device_queue::downloaded_fields() const noexcept {
  return downloaded_;
}

size_t device_queue::barrier_count() const noexcept {
  return barriers_;
}

size_t device_queue::upload_byte_count() const noexcept {
  return upload_bytes_;
}

size_t device_queue::download_byte_count() const noexcept {
  return download_bytes_;
}

size_t device_queue::image_count() const noexcept {
  size_t total = 0;
  for (const auto& slot : slots_) {
    total += size_t(slot.residence == device_residence::in_image);
  }
  return total;
}

bool device_queue::is_image(const std::string_view& field) const noexcept {
  for (const auto& slot : slots_) {
    if (field_name_of(slot.field) != field) continue;
    return slot.residence == device_residence::in_image;
  }
  return false;
}

device_report device_queue::run() {
  return execute(true);
}

device_report device_queue::run_step_by_step() {
  return execute(false);
}

device_report device_queue::execute(const bool single_submission) {
  device_report report;
  report.calls = plans_.size();
  report.barriers = barriers_;
  report.upload_bytes = upload_bytes_;
  report.download_bytes = download_bytes_;
  report.images = image_count();

  // Хостовые байты поля лежат подряд: раскладка `soa` проверена планом, поэтому передача — это
  // memcpy, а не сборка со страйдом.
  for (const auto index : upload_slots_) {
    const auto& slot = slots_[index];
    const auto accessor = slot.field.read();
    context_->write(slot.staging, accessor.data(), slot.byte_size);
  }

  const auto record_start = std::chrono::steady_clock::now();

  // Загрузка и выгрузка отличаются у картинки только видом копии: перевалочный буфер тот же, потому
  // что на хосте поле в любом случае плотный отрезок байт.
  const auto record_upload = [&](painter::compute_context::recorder& rec, const size_t index) {
    const auto& slot = slots_[index];
    if (slot.residence == device_residence::in_image) {
      rec.copy_to_image(slot.staging, slot.device_image);
      return;
    }
    rec.copy(slot.staging, slot.device_buffer, slot.byte_size);
  };
  const auto record_download = [&](painter::compute_context::recorder& rec, const size_t index) {
    const auto& slot = slots_[index];
    if (slot.residence == device_residence::in_image) {
      rec.copy_from_image(slot.device_image, slot.staging);
      return;
    }
    rec.copy(slot.device_buffer, slot.staging, slot.byte_size);
  };

  // ОДНОЙ ОТПРАВКОЙ. Загрузка, все проходы и выгрузка уходят вместе — это и есть «исполняется
  // целиком, без возврата в оркестратор», верное и на уровне Vulkan.
  const auto submit_start = std::chrono::steady_clock::now();
  if (single_submission) {
    context_->submit([&](painter::compute_context::recorder& rec) {
      for (const auto index : upload_slots_) {
        record_upload(rec, index);
      }
      if (!upload_slots_.empty()) {
        rec.barrier();
      }

      for (const auto& plan : plans_) {
        if (plan.barrier_before) {
          rec.barrier();
        }
        rec.dispatch(plan.program, plan.set, plan.push.data(), plan.push.size(), plan.element_count);
      }

      if (!download_slots_.empty()) {
        rec.barrier();
      }
      for (const auto index : download_slots_) {
        record_download(rec, index);
      }
    });
  } else {
    // Каждый шаг — своя отправка, то есть своё ожидание fence. Барьеров здесь не нужно вовсе:
    // ожидание отправки и есть полная точка синхронизации. Именно поэтому такой способ и выглядит
    // корректным, пока не посмотришь на цену.
    for (const auto index : upload_slots_) {
      context_->submit([&](painter::compute_context::recorder& rec) { record_upload(rec, index); });
    }
    for (const auto& plan : plans_) {
      context_->submit([&](painter::compute_context::recorder& rec) {
        rec.dispatch(plan.program, plan.set, plan.push.data(), plan.push.size(), plan.element_count);
      });
    }
    for (const auto index : download_slots_) {
      context_->submit([&](painter::compute_context::recorder& rec) { record_download(rec, index); });
    }
  }
  const double submit_ms =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_start).count();

  for (const auto index : download_slots_) {
    const auto& slot = slots_[index];
    const auto accessor = slot.field.write();
    if (accessor.data() == nullptr) {
      utils::error{}("originator step '{}': '{}' is named in output but bound for reading",
                     queue_.name, field_name_of(slot.field));
    }
    context_->read(slot.staging, accessor.data(), slot.byte_size);
  }

  report.record_ms =
    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - record_start).count();
  report.submit_ms = submit_ms;
  return report;
}

} // namespace originator
} // namespace devils_engine
