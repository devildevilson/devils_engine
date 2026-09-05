#ifndef DEVILS_ENGINE_ORIGINATOR_DEVICE_EXECUTOR_H
#define DEVILS_ENGINE_ORIGINATOR_DEVICE_EXECUTOR_H

#include <memory>
#include <string>
#include <vector>

#include "device_queue.h"

// ИСПОЛНИТЕЛЬ УСТРОЙСТВЕННЫХ ОЧЕРЕДЕЙ для тела шага.
//
// Очередь объявляет `on_device` в конфиге (§6.4): решение о пути принимает МАНИФЕСТ, а не машина,
// поэтому все машины берут одну ветку, а та, что исполнить её не может, отказывает громко. Ставит
// исполнитель тот, кто собрал приложение, — ядро генератора про Vulkan не знает.
//
// ПЛАН СТРОИТСЯ ОДИН РАЗ И ПЕРЕИСПОЛЬЗУЕТСЯ. У стримингового генератора один и тот же шаг зовётся в
// каждом чанке; строить план заново значило бы каждый раз заводить буферы устройства и наборы
// дескрипторов, а компиляцию шейдера спасал бы только кэш по тексту. Поэтому планы держатся по
// ПОДПИСИ очереди — по её структуре, а не по числам: параметры переписываются в push-константу перед
// каждой отправкой, и смена порога не строит второй план.

namespace devils_engine {
namespace originator {

class device_executor : public queue_executor {
public:
  explicit device_executor(painter::compute_context& context) noexcept;
  ~device_executor() noexcept override;

  device_executor(const device_executor&) = delete;
  device_executor& operator=(const device_executor&) = delete;

  bool can_run(const computation_queue& queue, std::string& refusal) override;
  queue_report run(const computation_queue& queue) override;

  // Сколько РАЗНЫХ планов построено и сколько раз они переиспользованы. Величины существуют затем,
  // чтобы переиспользование можно было проверить, а не предположить: план, строящийся заново на
  // каждый вызов, считает то же самое.
  size_t plan_count() const noexcept;
  size_t reuse_count() const noexcept;

  // Последний исполненный план: им пользуются замеры и проверки, которым нужно посмотреть на
  // выведенную передачу, а не только на результат.
  const device_queue* last_plan() const noexcept;
  const device_report& last_report() const noexcept;

private:
  struct entry;

  painter::compute_context* context_ = nullptr;
  std::vector<entry> plans_;
  size_t reuse_ = 0;
  const device_queue* last_ = nullptr;
  device_report last_report_{};
};

} // namespace originator
} // namespace devils_engine

#endif
