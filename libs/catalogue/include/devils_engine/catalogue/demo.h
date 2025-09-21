#ifndef DEVILS_ENGINE_CATALOGUE_REGISTRY_H
#define DEVILS_ENGINE_CATALOGUE_REGISTRY_H

#include <cstdint>
#include <cstddef>
#include <string>
#include "common.h"

namespace devils_engine {
namespace catalogue {
struct demo {
  enum class consume_type {
    none,
    input,
    mutator,
    // meta? полезно тоже собрать
    all
  };

  class input_consumer : public consumer {
  public:
    demo* cur;

    input_consumer(demo* cur) noexcept;
    void consume(const buffer& b) override;
  };

  class mutator_consumer : public consumer {
  public:
    demo* cur;

    mutator_consumer(demo* cur) noexcept;
    void consume(const buffer& b) override;
  };

  // цель? создать полный список действий игрока и воспроизвести их в точности
  // чтобы отрендерить демо сцену, тут по логике я должен собрать просто мутаторы
  // затем в основном цикле последовательно применять изменения + рисовать кадры на экран
  // вообще нет я могу сделать демо 2 способами:
  // 1. собрать мутаторы
  // 2. собрать инпуты
  // ну и собрать и то и другое где то в одном месте
  // способ 1 даст мне меньший размер файла + возможность прогнать инпуты и пересимулировать полностью мир
  // что в свою очередь даст возможность прикинуть производительность сцен
  // способ 2 даст больший размер файла, но при этом более устойчив к изменению версии
  // если базовые API функции не изменятся то не изменится и результат (предположительно)
  // вообще имеет смысл собрать и то и другое для демо

  struct storage {
    std::vector<buffer> input_buffer;
    std::vector<buffer> mutator_buffer;
  };

  input_consumer c1;
  mutator_consumer c2;

  struct storage storage;

  demo(const consume_type t = consume_type::all) noexcept;

  bool write_to_disk(const std::string &path) const;
  bool load_from_disk(const std::string& path);
};

}
}

#endif