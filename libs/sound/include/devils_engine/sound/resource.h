#ifndef DEVILS_ENGINE_SOUND_RESOURCE_H
#define DEVILS_ENGINE_SOUND_RESOURCE_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "common.h"
#include "decoder.h"

namespace devils_engine {
namespace sound {

// тут не помешает наследоваться от demiurg resource_interface
// ну или придумать как то иначе, чтобы не хранить лишние строки

#define SOUND_SYSTEM_EXTENSION_LIST \
  X(mp3)                            \
  X(flac)                           \
  X(wav)                            \
  X(ogg)                            \
  X(opus)                           \
  X(pcm)

struct resource {
  enum class type {
#define X(name) name,
    SOUND_SYSTEM_EXTENSION_LIST
#undef X

      undefined
  };

  static std::string_view type_to_string(const size_t index);

  std::string id;
  enum type type;
  std::unique_ptr<sound::decoder> sound;
  std::vector<char> buffer; // буфер обязательно должен присустсвовать для некоторых декодеров

  resource();
  // звуки меньше 5сек переводятся в pcm автоматически
  resource(std::string id, enum type type, std::vector<char> buffer);
  ~resource() noexcept;

  double duration() const; // s
};

// Immutable backing store одного опубликованного поколения sound_resource. Ресурс атомарно
// заменяет/снимает свою ссылку при load/unload, а broker-команда и playback task держат shared pin.
// Поэтому старое поколение живёт до завершения последней уже принятой задачи и никогда не меняется
// под декодером.
struct resource_blob {
  std::string id;
  enum data_type type = data_type::undefined;
  std::vector<char> data;
  enum format sample_format = format::unknown;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  size_t frames_count = 0;
};

// Легковесный playback view. Если owner непустой, он владеет памятью data и строкой id.
// Ручные non-owning views допустимы только для локальных fixtures с внешне гарантированным lifetime.
struct resource2 {
  std::shared_ptr<const resource_blob> owner;
  std::string_view id;
  enum data_type type = data_type::undefined;
  std::span<const char> data = {};
  // Метаданные исходного аудио. Заполняются ВСЕГДА (полезны и demiurg::resource, и микшеру). Для
  // type==pcm data — это уже сырые декодированные кадры данного формата/каналов/частоты (короткие
  // звуки декодируются в ресурсе целиком, чтобы микшер не декодил per-frame; см. sound_resource).
  enum format sample_format = format::unknown;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  size_t frames_count = 0;

  resource2() noexcept = default;
  explicit resource2(std::shared_ptr<const resource_blob> blob) noexcept;
  bool pinned() const noexcept;
};
} // namespace sound
} // namespace devils_engine

#endif
