#include "devils_engine/originator/device_queue.h"

#include <algorithm>
#include <chrono>
#include <cstring>
#include <format>

#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

namespace {
// Поле, которое очередь трогает. Одно поле — один буфер устройства: у `soa` поле лежит подряд, и
// такой буфер отображается в `float data[]` шейдера один в один, без страйда и без перестановки.
struct touched_field {
  field_ref field{};
  bool read_before_written = false;
  bool written = false;
};

std::string field_name_of(const field_ref& field) {
  return std::format("{}.{}", field.buffer_name(), field.field_name());
}

// Род поля, у которого на устройстве есть представление. Узкие роды существуют ради КОМПАКТНОГО
// хранения на CPU, а считать в них никто и не собирался (§3.2), поэтому ограничение почти не задевает
// того, для чего они заведены.
bool device_field_kind(const field_type type) noexcept {
  if (type.components != 1) {
    return false;
  }
  return type.base == field_base::v || type.base == field_base::ui || type.base == field_base::i;
}

std::string_view refuse_field(const field_ref& field) noexcept {
  const auto type = field.type();
  if (type.components != 1) {
    return "a multi-component field has no single-typed shader buffer";
  }
  return "the queue takes the 32-bit kinds v, ui and i on a device; the narrow kinds exist for compact "
         "storage on the host, and computing in them was never the point";
}
} // namespace

struct device_queue::field_slot {
  field_ref field{};
  painter::compute_context::buffer_id device_buffer = painter::compute_context::invalid_id;
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

  for (size_t i = 0; i < queue.calls.size(); ++i) {
    const auto& call = queue.calls[i];
    const size_t position = i + 1;

    const bool native = call.tool != nullptr;
    const auto& source = native ? call.tool->device_source : call.device_source;
    if (source.empty()) {
      const auto reason = native
                            ? std::string("the tool declares no device form")
                            : (call.device_refusal.empty() ? std::string("the call carries no device form")
                                                           : call.device_refusal);
      return fail(std::format("step '{}': queue element {} ('{}') does not run on a device: {}",
                              queue.name, position, call.label, reason));
    }

    for (const auto& binding : call.inputs) {
      if (device_field_kind(binding.type())) continue;
      return fail(std::format("step '{}': queue element {} ('{}') reads '{}', and {}",
                              queue.name, position, call.label, field_name_of(binding), refuse_field(binding)));
    }
    for (const auto& binding : call.outputs) {
      if (device_field_kind(binding.type())) continue;
      return fail(std::format("step '{}': queue element {} ('{}') writes '{}', and {}",
                              queue.name, position, call.label, field_name_of(binding), refuse_field(binding)));
    }

    // РАСКЛАДКА ОБЯЗАНА БЫТЬ `soa`, и это не придирка. Поле `aos` лежит с шагом элемента, то есть
    // непрерывным блоком его не передать, а страйд на устройстве стоит линейно по шагу (измерено:
    // шаг 4 — вдвое-втрое, шаг 8 — вчетверо-впятеро). Значит выбор `aos` для устройственной очереди
    // означал бы и лишний код передачи, и заведомо худший доступ.
    const auto layout_ok = [&](const field_ref& binding) {
      return binding.source != nullptr && binding.source->layout().storage == storage_kind::soa;
    };
    for (const auto& binding : call.inputs) {
      if (layout_ok(binding)) continue;
      return fail(std::format("step '{}': buffer '{}' is laid out as aos, and a device queue needs soa — the "
                              "field must lie contiguously to be transferred, and a stride costs linearly on a "
                              "device",
                              queue.name, binding.buffer_name()));
    }
    for (const auto& binding : call.outputs) {
      if (layout_ok(binding)) continue;
      return fail(std::format("step '{}': buffer '{}' is laid out as aos, and a device queue needs soa",
                              queue.name, binding.buffer_name()));
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

  // 1. ЧТО ТРОГАЕТ ОЧЕРЕДЬ. Обход тот же, которым `check_queue` ищет мёртвую работу: поле, которое
  //    вызов читает и которого до него никто не писал, — это ВХОД, а что уезжает наружу, сказано в
  //    `output`. Оба списка вычисляются, а не объявляются.
  std::vector<touched_field> touched;
  const auto slot_of = [&](const field_ref& field) -> size_t {
    for (size_t i = 0; i < touched.size(); ++i) {
      if (touched[i].field.same_field_as(field)) return i;
    }
    touched.push_back(touched_field{field, false, false});
    return touched.size() - 1;
  };

  for (const auto& call : queue_.calls) {
    for (const auto& binding : call.inputs) {
      const auto index = slot_of(binding);
      if (!touched[index].written) {
        touched[index].read_before_written = true;
      }
    }
    if (call.indirect()) {
      const auto index = slot_of(call.count_from);
      if (!touched[index].written) {
        touched[index].read_before_written = true;
      }
    }
    for (const auto& binding : call.outputs) {
      touched[slot_of(binding)].written = true;
    }
  }

  slots_.reserve(touched.size());
  for (const auto& entry : touched) {
    field_slot slot;
    slot.field = entry.field;
    slot.byte_size = entry.field.count() * entry.field.type().byte_size();
    slot.device_buffer = context_->create_buffer(slot.byte_size, false);
    slots_.push_back(slot);
  }

  for (size_t i = 0; i < touched.size(); ++i) {
    if (!touched[i].read_before_written) continue;
    slots_[i].staging = context_->create_buffer(slots_[i].byte_size, true);
    upload_slots_.push_back(i);
    uploaded_.push_back(field_name_of(touched[i].field));
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
    const bool native = call.tool != nullptr;
    const auto& source = native ? call.tool->device_source : call.device_source;
    const auto& params = native ? call.tool->device_params : call.device_params;

    const uint32_t binding_count = uint32_t(call.inputs.size() + call.outputs.size());
    const uint32_t push_size = uint32_t(sizeof(device_call_header) + params.size() * sizeof(float));

    call_plan plan;
    plan.program = context_->create_program(call.label, source, binding_count, push_size);
    plan.set = context_->create_binding_set(plan.program);

    std::vector<painter::compute_context::bound_resource> resources;
    resources.reserve(binding_count);
    for (const auto& binding : call.inputs) {
      resources.push_back(
        painter::compute_context::bound_resource::of_buffer(slots_[slot_of(binding)].device_buffer));
    }
    for (const auto& binding : call.outputs) {
      resources.push_back(
        painter::compute_context::bound_resource::of_buffer(slots_[slot_of(binding)].device_buffer));
    }
    context_->update_binding_set(plan.set, resources);

    // 3. PUSH-КОНСТАНТА по ОБЩЕМУ соглашению: шапка, затем объявленные параметры по одному float в
    //    объявленном порядке. Один способ выложить байты на все инструменты и все переводы — два
    //    способа однажды разъехались бы, а шейдер прочитал бы чужие числа и не пожаловался.
    plan.push.resize(push_size);
    device_call_header header;
    header.count = uint32_t(call.range_count());
    header.begin = uint32_t(call.range_begin);
    const auto& extent = call.outputs.front().extent();
    header.extent_x = uint32_t(extent.declared() ? extent.x : call.outputs.front().count());
    header.extent_y = uint32_t(extent.declared() && extent.y != 0 ? extent.y : 1);
    std::memcpy(plan.push.data(), &header, sizeof(header));

    for (size_t p = 0; p < params.size(); ++p) {
      // Значение по умолчанию берётся у САМОГО инструмента через его же чтение параметров: числа,
      // которых в вызове нет, обязаны совпасть с тем, что подставил бы CPU.
      const float value = float(call.params.number(params[p].name, params[p].fallback));
      std::memcpy(plan.push.data() + sizeof(header) + p * sizeof(float), &value, sizeof(value));
    }

    plan.element_count = call.range_count();

    // 4. БАРЬЕР ВЫВОДИТСЯ. Тот же вопрос, что у проверки мёртвой работы: читает ли этот проход то,
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

const std::vector<std::string>& device_queue::uploaded_fields() const noexcept {
  return uploaded_;
}

const std::vector<std::string>& device_queue::downloaded_fields() const noexcept {
  return downloaded_;
}

size_t device_queue::barrier_count() const noexcept {
  return barriers_;
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

  // Хостовые байты поля лежат подряд: раскладка `soa` проверена планом, поэтому передача — это
  // memcpy, а не сборка со страйдом.
  for (const auto index : upload_slots_) {
    const auto& slot = slots_[index];
    const auto accessor = slot.field.read();
    context_->write(slot.staging, accessor.data(), slot.byte_size);
  }

  const auto record_start = std::chrono::steady_clock::now();
  double submit_ms = 0.0;

  // ОДНОЙ ОТПРАВКОЙ. Загрузка, все проходы и выгрузка уходят вместе — это и есть «исполняется
  // целиком, без возврата в оркестратор», верное и на уровне Vulkan.
  const auto submit_start = std::chrono::steady_clock::now();
  if (single_submission) {
    context_->submit([&](painter::compute_context::recorder& rec) {
      for (const auto index : upload_slots_) {
        rec.copy(slots_[index].staging, slots_[index].device_buffer, slots_[index].byte_size);
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
        rec.copy(slots_[index].device_buffer, slots_[index].staging, slots_[index].byte_size);
      }
    });
  } else {
    // Каждый шаг — своя отправка, то есть своё ожидание fence. Барьеров здесь не нужно вовсе:
    // ожидание отправки и есть полная точка синхронизации. Именно поэтому такой способ и выглядит
    // корректным, пока не посмотришь на цену.
    for (const auto index : upload_slots_) {
      context_->submit([&](painter::compute_context::recorder& rec) {
        rec.copy(slots_[index].staging, slots_[index].device_buffer, slots_[index].byte_size);
      });
    }
    for (const auto& plan : plans_) {
      context_->submit([&](painter::compute_context::recorder& rec) {
        rec.dispatch(plan.program, plan.set, plan.push.data(), plan.push.size(), plan.element_count);
      });
    }
    for (const auto index : download_slots_) {
      context_->submit([&](painter::compute_context::recorder& rec) {
        rec.copy(slots_[index].device_buffer, slots_[index].staging, slots_[index].byte_size);
      });
    }
  }
  submit_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - submit_start).count();

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
