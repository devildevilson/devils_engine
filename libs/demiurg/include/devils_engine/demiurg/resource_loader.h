#ifndef DEVILS_ENGINE_DEMIURG_RESOURCE_LOADER_H
#define DEVILS_ENGINE_DEMIURG_RESOURCE_LOADER_H

#include <cstddef>
#include <cstdint>
#include <vector>

#include "resource_base.h"

// resource_loader — мозг загрузки ресурсов. Снаружи в него поступают запросы вида
// «доведи ресурс X до состояния target» (cold/warm/hot). Он сводит фактическое
// состояние ресурса к target по одному переходу за тик, схлопывая противоречивые/
// избыточные запросы к последнему target и отбрасывая ложные.
//
// Переходы cold<->warm (диск/CPU) loader исполняет САМ, на вызывающем (ассетном) потоке.
// Переходы warm<->hot затрагивают GPU/дескриптор — их loader делать не может (см.
// архитектурное решение: финальная GPU-таблица живёт на потоке рендера). Такой переход
// loader отдаёт наружу как external_job; владелец (актор ассетов) сливает их в рендер,
// помечает ресурс in-flight и по завершении зовёт external_done(). Эвристика «что есть
// GPU-переход»: если у ресурса НЕ выставлен флаг warm_and_hot_same, то warm<->hot внешний.
//
// Для тяжёлых render resources (например pipeline) правильный staged contract такой:
// assets-поток делает CPU-heavy prepare/compile в локальных шагах ресурса, а внешний шаг
// только публикует/коммитит render-owned GPU handles. VkPipeline технически можно создать
// с другого потока при корректной синхронизации VkDevice/VkPipelineCache, но владение и
// уничтожение graph-local handles остаётся проще и безопаснее держать на render side.
//
// Класс однопоточный: предполагается, что весь менеджмент крутится на одном акторе ассетов.

namespace devils_engine {
namespace demiurg {

class resource_loader {
public:
  // Переход, который loader не может выполнить локально (warm<->hot, GPU-сторона).
  struct external_job {
    resource_interface* res;
    bool load; // true: warm->hot (load_warm); false: hot->warm (unload_hot)
  };

  resource_loader() noexcept;
  ~resource_loader() noexcept;

  // Задать/заменить желаемое состояние ресурса (уровень FSM; cold/warm/hot = 0/1/2, но
  // многошаговые ресурсы имеют больше уровней). target клампится к res->final_state().
  // Побеждает последний вызов. Идемпотентно; если ресурс уже в target — запрос игнорируется.
  void request(resource_interface* res, int32_t target);

  // Свести по одному шагу каждый незавершённый ресурс к его target на вызывающем потоке.
  // Локальные переходы исполняются здесь же; warm<->hot складываются в out и ресурс
  // помечается in-flight до external_done(). Возвращает число ресурсов не в target.
  size_t update(std::vector<external_job>& out);

  // Вызвать после того, как рендер завершил external_job по ресурсу (рендер уже флипнул
  // _state). Снимает пометку in-flight — на следующем update reconcile продолжится.
  void external_done(resource_interface* res);

  size_t pending_count() const noexcept;
  void clear() noexcept;

private:
  struct entry {
    resource_interface* res;
    int32_t target;
    bool in_flight;
  };

  std::vector<entry> entries;

  entry* find(resource_interface* res) noexcept;
};

}
}

#endif
