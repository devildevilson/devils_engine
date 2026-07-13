#ifndef DEVILS_ENGINE_THREAD_BYTE_RING_H
#define DEVILS_ENGINE_THREAD_BYTE_RING_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace devils_engine {
namespace thread {

// SPSC байт-арена (bip-buffer) под payload сообщений, спаренная с spsc_queue<msg>.
//
// Продюсер bump'ает head (alloc), консьюмер bump'ает tail (release) — строго FIFO. Аллокация
// payload'ов и потребление соответствующих сообщений идут в ОДНОМ порядке (одна очередь сообщений
// на канал), поэтому освобождение = монотонный курсор tail, БЕЗ пер-блочного free и отложенного
// учёта.
//
// Позиции МОНОТОННЫЕ (uint64, не модульные): при завороте, если payload не влезает в хвост
// contiguous, продюсер пропускает "паддинг" до начала буфера — и т.к. позиция всё равно растёт,
// этот паддинг реклеймится сам, когда tail проходит конец сообщения. Модульный индекс — только для
// доступа к памяти (pos % capacity).
//
// СИНХРОНИЗАЦИЯ: атомарен ТОЛЬКО tail (консьюмер публикует release, продюсер читает acquire в alloc
// для расчёта свободного места). head — локальный для продюсера. Видимость самих байт payload'а
// обеспечивает release/acquire ОЧЕРЕДИ СООБЩЕНИЙ: payload пишется ДО push(msg), читается ПОСЛЕ
// pop(msg). Поэтому byte_ring не публикует head сам.
//
// Контракт консьюмера: payload по позиции из сообщения ДЕЙСТВИТЕЛЕН только до release этого
// сообщения. Нужен дольше — скопируй наружу перед release.
class byte_ring {
public:
  explicit byte_ring(const size_t capacity);

  size_t capacity() const noexcept;

  // ПРОДЮСЕР. Зарезервировать n contiguous байт. Успех: возвращает монотонную позицию (>=0) и пишет
  // в out доступную для записи область. Неудача (overflow / n > capacity): возвращает -1, out пуст.
  // Политику на overflow решает вызывающий (drop / grow / spill).
  int64_t alloc(const size_t size, std::span<std::byte>& out) noexcept;

  // КОНСЬЮМЕР. Область payload'а по (pos, n) из сообщения. Всегда contiguous (гарантировано alloc).
  std::span<const std::byte> at(const int64_t pos, const size_t size) const noexcept;

  // КОНСЬЮМЕР. Освободить всё вплоть до конца обработанного сообщения (передавать pos + n).
  // Монотонно (сообщения потребляются по порядку). Заодно реклеймит паддинг перед payload'ом.
  void release(const int64_t pos_end) noexcept;

  // Диагностика (приблизительно): сколько байт занято (не реклеймнуто).
  size_t used_approx() const noexcept;

private:
  std::vector<std::byte> buffer;
  uint64_t head_ = 0;                          // producer-local
  alignas(64) std::atomic<uint64_t> tail_ = 0; // consumer publishes, producer reads
};

} // namespace thread
} // namespace devils_engine

#endif
