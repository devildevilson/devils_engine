#ifndef DEVILS_ENGINE_THREAD_MAILBOX_H
#define DEVILS_ENGINE_THREAD_MAILBOX_H

#include <atomic>
#include <cstdint>

namespace devils_engine {
namespace thread {

// Latest-wins SPSC-мейлбокс (triple-buffer): консьюмер всегда получает САМОЕ СВЕЖЕЕ опубликованное
// значение, продюсер никогда не блокируется, drop-oldest — штатный режим. Для каналов, где важно
// только последнее (камера, ресайз, снапшоты draw_*). Три слота: продюсер заполняет свой НА МЕСТЕ
// (capacity T переиспользуется между кадрами ⇒ ноль пер-кадровых аллокаций для крупных payload'ов
// вроде std::vector в T), затем publish() атомарно обменивает его с общим слотом.
//
// SPSC: один продюсер зовёт write_slot()/publish(), один консьюмер зовёт consume().
template <typename T>
class mailbox {
public:
  mailbox() = default;

  // ПРОДЮСЕР. Ссылка на слот для заполнения НА МЕСТЕ (переиспользует ёмкость). Валидна до publish().
  T& write_slot() noexcept {
    return slots_[producer_idx_];
  }

  // ПРОДЮСЕР. Опубликовать заполненный write_slot как самый свежий.
  void publish() noexcept {
    const uint32_t prev = shared_.exchange(producer_idx_ | fresh_bit, std::memory_order_release);
    producer_idx_ = prev & idx_mask;
  }

  // КОНСЬЮМЕР. Указатель на самое свежее опубликованное значение, либо nullptr — если с прошлого
  // consume() ничего нового не публиковалось. Возвращённый указатель валиден до следующего consume().
  const T* consume() noexcept {
    if ((shared_.load(std::memory_order_acquire) & fresh_bit) == 0) {
      return nullptr;
    }
    const uint32_t prev = shared_.exchange(consumer_idx_, std::memory_order_acquire);
    consumer_idx_ = prev & idx_mask;
    return &slots_[consumer_idx_];
  }

private:
  static constexpr uint32_t idx_mask = 0x3;
  static constexpr uint32_t fresh_bit = 0x4;

  T slots_[3];
  uint32_t producer_idx_ = 0;
  uint32_t consumer_idx_ = 1;
  alignas(64) std::atomic<uint32_t> shared_ = 2; // индекс 2 в общем слоте, флаг fresh снят
};

} // namespace thread
} // namespace devils_engine

#endif
