#ifndef DEVILS_ENGINE_DEMIURG_SYNCHRONOUS_LOADER_H
#define DEVILS_ENGINE_DEMIURG_SYNCHRONOUS_LOADER_H

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

#include "resource_loader.h"

// synchronous_loader — «поток ассетов» контекста, у которого потока ассетов нет: headless-сервер,
// тесты, инструменты. Тот же resource_loader (зависимости, порядок переходов), только сведение идёт
// до конца прямо на вызывающем потоке. Внешних (GPU) шагов у такого контекста нет: ресурс, которому
// нужен внешний шаг до target, — критическая ошибка вызывающего.
//
// Контекст с потоком ассетов этим загрузчиком НЕ пользуется: его ресурсы запрашивает состояние
// загрузки через канал load_resource, а потребитель только проверяет готовность. Свой реестр —
// свой загрузчик; два загрузчика над одним реестром — гонка.

namespace devils_engine {
namespace demiurg {

class resource_system;

class synchronous_loader {
public:
  // Довести ресурс и его зависимости до target (клампится к final_state).
  void load(resource_interface* res, int32_t target);
  // То же до final_state: ресурс usable.
  void load(resource_interface* res);
  // Все ресурсы каталога prefix (id начинается с prefix) до final_state. Возвращает их число;
  // пустой каталог — решение вызывающего.
  size_t load_directory(const resource_system& resources, std::string_view prefix);

private:
  void settle(resource_interface* res, int32_t target);

  resource_loader loader_;
  std::vector<resource_loader::external_job> jobs_;
};

} // namespace demiurg
} // namespace devils_engine

#endif
