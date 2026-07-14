#ifndef DEVILS_ENGINE_AESTHETICS_MESSAGE_REGISTRY_H
#define DEVILS_ENGINE_AESTHETICS_MESSAGE_REGISTRY_H

#include <cstddef>
#include <memory>
#include <vector>

#include "common.h"
#include "message_buffer.h"

// message_registry — типизированная шина сообщений пайплайна: набор per-type каналов (message_buffer),
// адресуемых монотонным message_type_id<Msg>. Это ОБЪЕКТ, который передаётся от системы к системе:
// система-продюсер пишет каналы своих выходных типов, система-консюмер читает каналы входных. Ось
// (message_type) дизайна «indexed by (message_type, entityid)» живёт здесь (тип → канал), ось entity —
// слот внутри message_buffer. Каналы создаются лениво при первом channel<Msg>() и переиспользуются
// между тиками (reset_all/clear_all — до/между фазами), поэтому реестр — долгоживущее состояние
// пайплайна, а не пер-тик аллокация. Хранилище type-erased (basic_channel/typed_channel<Msg>), чтобы
// в одном контейнере жили каналы разных типов; ссылки на каналы стабильны (unique_ptr-слоты).

namespace devils_engine {
namespace aesthetics {

class message_registry {
public:
  // Канал типа Msg (создаётся при первом обращении). Ссылка стабильна между вызовами.
  template <typename Msg>
  message_buffer<Msg>& channel() {
    const size_t id = message_type_id<Msg>();
    if (channels_.size() <= id) {
      channels_.resize(id + 1);
    }
    auto& slot = channels_[id];
    if (!slot) {
      slot = std::make_unique<typed_channel<Msg>>();
    }
    return static_cast<typed_channel<Msg>*>(slot.get())->buffer;
  }

  // Существующий канал типа Msg или nullptr (не создаёт). Для консюмеров, которым канала может не быть.
  template <typename Msg>
  const message_buffer<Msg>* find() const {
    const size_t id = message_type_id<Msg>();
    if (id >= channels_.size() || !channels_[id]) {
      return nullptr;
    }
    return &static_cast<const typed_channel<Msg>*>(channels_[id].get())->buffer;
  }

  template <typename Msg>
  message_buffer<Msg>* find() {
    const size_t id = message_type_id<Msg>();
    if (id >= channels_.size() || !channels_[id]) {
      return nullptr;
    }
    return &static_cast<typed_channel<Msg>*>(channels_[id].get())->buffer;
  }

  template <typename Msg>
  bool has() const {
    return find<Msg>() != nullptr;
  }

  // Задать ёмкость КАЖДОГО существующего канала (обычно world::index_capacity()) — сайзинг на главном
  // потоке перед параллельной фазой продюсеров, чтобы store не реаллоцировал.
  void reset_all(const size_t capacity) {
    for (auto& c : channels_) {
      if (c) {
        c->reset(capacity);
      }
    }
  }

  // Очистить присутствие в КАЖДОМ канале, сохранив ёмкость/память (между фазами одного тика).
  void clear_all() noexcept {
    for (auto& c : channels_) {
      if (c) {
        c->clear();
      }
    }
  }

private:
  struct basic_channel {
    virtual ~basic_channel() noexcept = default;
    virtual void reset(size_t capacity) = 0;
    virtual void clear() noexcept = 0;
  };

  template <typename Msg>
  struct typed_channel : basic_channel {
    message_buffer<Msg> buffer;
    void reset(const size_t capacity) override {
      buffer.reset(capacity);
    }
    void clear() noexcept override {
      buffer.clear();
    }
  };

  std::vector<std::unique_ptr<basic_channel>> channels_; // индекс = message_type_id<Msg>
};

} // namespace aesthetics
} // namespace devils_engine

#endif
