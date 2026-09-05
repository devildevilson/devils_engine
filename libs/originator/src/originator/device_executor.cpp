#include "devils_engine/originator/device_executor.h"

#include <format>

#include "devils_engine/utils/core.h"

namespace devils_engine {
namespace originator {

namespace {
// ПОДПИСЬ ОЧЕРЕДИ — её структура, и только структура. Числа параметров сюда НЕ входят намеренно: они
// переписываются в push-константу перед отправкой, а если бы они попали в ключ, смена одного порога
// строила бы второй план со своими буферами устройства.
//
// Зато входят все привязки по ТОЖДЕСТВУ (адрес буфера и номер поля): та же очередь над другими
// буферами — другой план, потому что дескрипторы принадлежат вызову.
std::string signature_of(const computation_queue& queue) {
  std::string text = queue.name;
  text.push_back('\n');

  const auto add_field = [&](const field_ref& field) {
    text.append(std::format("{}:{}:{};", static_cast<const void*>(field.source), field.field_index,
                            field.writable() ? 'w' : 'r'));
  };

  for (const auto& call : queue.calls) {
    text.append(std::format("{}|{}|{}|{}|{}|", call.label, static_cast<const void*>(call.tool),
                            uint32_t(call.shape), call.range_begin, call.range_end));
    for (const auto& binding : call.inputs) add_field(binding);
    text.push_back('>');
    for (const auto& binding : call.outputs) add_field(binding);
    // Тело чужого вызова — часть структуры: другой перевод это другая программа.
    text.append(call.device.body());
    text.push_back('\n');
  }

  text.append("out:");
  for (const auto& binding : queue.output) add_field(binding);
  text.append("resident:");
  for (const auto& binding : queue.resident) add_field(binding);
  return text;
}
} // namespace

struct device_executor::entry {
  std::string signature;
  std::unique_ptr<device_queue> plan;
};

device_executor::device_executor(painter::compute_context& context) noexcept : context_(&context) {}

device_executor::~device_executor() noexcept = default;

bool device_executor::can_run(const computation_queue& queue, std::string& refusal) {
  const auto check = check_device_queue(queue);
  refusal = check.message;
  return check.allowed;
}

queue_report device_executor::run(const computation_queue& queue) {
  const auto signature = signature_of(queue);

  device_queue* plan = nullptr;
  for (auto& known : plans_) {
    if (known.signature != signature) continue;
    plan = known.plan.get();
    ++reuse_;
    break;
  }

  if (plan == nullptr) {
    auto built = std::make_unique<device_queue>(*context_, queue);
    plan = built.get();
    plans_.push_back(entry{signature, std::move(built)});
  }

  last_ = plan;
  last_report_ = plan->run(queue);

  queue_report report;
  report.calls = last_report_.calls;
  // ОБХОДОВ РОВНО СТОЛЬКО, СКОЛЬКО ВЫЗОВОВ. Слияние — приём CPU: оно держит промежуточное поле в
  // кэше, а у устройства кэш другой и обход другой, поэтому переставлять проходы здесь незачем.
  report.passes = last_report_.calls;
  report.fused = 0;
  report.clamped = 0;
  report.on_device = true;
  return report;
}

size_t device_executor::plan_count() const noexcept {
  return plans_.size();
}

size_t device_executor::reuse_count() const noexcept {
  return reuse_;
}

const device_queue* device_executor::last_plan() const noexcept {
  return last_;
}

const device_report& device_executor::last_report() const noexcept {
  return last_report_;
}

} // namespace originator
} // namespace devils_engine
