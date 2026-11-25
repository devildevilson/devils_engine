#ifndef DEVILS_ENGINE_NETWORK_DOCUMENT_CHUNKS_H
#define DEVILS_ENGINE_NETWORK_DOCUMENT_CHUNKS_H

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include <devils_engine/utils/serialization.h>

// ДОКУМЕНТ КУСКАМИ: checkpoint (или иной документ, чей размер не является размером сообщения) едет
// кусками по одной надёжной упорядоченной полосе.
//
// Кусок на проводе — сразу за байтом типа сообщения потребителя: u32 offset, u32 length, payload.
// Это единственный класс, несущий собственную 32-битную длину. Тип сообщения, бюджет куска и предел
// документа объявляет потребитель.
//
// Сборка (`document_assembly`) принимает куски строго по порядку: они покрывают документ один раз.
// Дубликат, перекрытие или дыра — отказ без изменения собранного. Объявленный размер проверяется
// ДО выделения памяти: чужое число не превращается в аллокацию в 4 ГиБ.

namespace devils_engine::network {

inline constexpr std::size_t document_chunk_header_bytes = 4 + 4;

struct document_chunk_view {
  uint32_t offset = 0;
  std::span<const std::byte> payload;
};

// Записать кусок (тип сообщения уже записан). Пустой кусок или длиннее max_payload — отказ.
[[nodiscard]] inline bool write_document_chunk(utils::serial::writer& w, const uint32_t offset,
                                               const std::span<const std::byte> payload,
                                               const std::size_t max_payload) {
  if (payload.empty() || payload.size() > max_payload) return false;
  w.u32(offset);
  w.u32(uint32_t(payload.size()));
  w.bytes(payload);
  return w.good();
}

// Прочитать кусок до конца сообщения (тип уже прочитан). Длина 0, больше max_payload или хвост после
// payload — отказ.
[[nodiscard]] inline bool read_document_chunk(utils::serial::reader& r, const std::size_t max_payload,
                                              document_chunk_view& out) {
  const auto offset = r.u32();
  const auto length = r.u32();
  if (!r.good() || length == 0 || length > max_payload) return false;
  const auto payload = r.take(length);
  if (!r.good() || r.position() != r.size()) return false;
  out.offset = offset;
  out.payload = payload;
  return true;
}

class document_assembly {
public:
  // Начать документ объявленного размера. 0 или больше max_bytes — отказ, память не выделяется.
  [[nodiscard]] bool begin(const uint32_t total_bytes, const uint32_t max_bytes) {
    if (total_bytes == 0 || total_bytes > max_bytes) return false;
    bytes_.assign(total_bytes, std::byte{0});
    received_ = 0;
    return true;
  }

  // Следующий кусок по порядку. Дубликат, перекрытие, дыра или выход за документ — отказ, собранное не
  // меняется.
  [[nodiscard]] bool append_in_order(const document_chunk_view& chunk) noexcept {
    if (received_ > bytes_.size() || chunk.offset != received_ || chunk.payload.empty() ||
        chunk.payload.size() > bytes_.size() - received_) {
      return false;
    }
    std::copy(chunk.payload.begin(), chunk.payload.end(), bytes_.begin() + received_);
    received_ += uint32_t(chunk.payload.size());
    return true;
  }

  bool started() const noexcept {
    return !bytes_.empty();
  }
  bool complete() const noexcept {
    return started() && received_ == bytes_.size();
  }
  uint32_t received() const noexcept {
    return received_;
  }
  uint32_t total() const noexcept {
    return uint32_t(bytes_.size());
  }
  std::span<const std::byte> bytes() const noexcept {
    return bytes_;
  }

  // Освободить память документа (после загрузки или отказа). received() остаётся: сколько байт пришло —
  // отчёт о передаче; следующий begin его сбрасывает.
  void release() noexcept {
    bytes_.clear();
    bytes_.shrink_to_fit();
  }

private:
  std::vector<std::byte> bytes_;
  uint32_t received_ = 0;
};

} // namespace devils_engine::network

#endif
