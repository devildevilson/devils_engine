#ifndef DEVILS_ENGINE_SOUND_SOUND_RESOURCE_H
#define DEVILS_ENGINE_SOUND_SOUND_RESOURCE_H

#include <cstdint>
#include <span>
#include <vector>

#include "devils_engine/demiurg/resource_base.h"
#include "resource.h" // resource2 + data_type (через common.h)

namespace devils_engine {
namespace sound {

// Звук как demiurg-ресурс: управляется потоком ассетов (как mesh/texture). Стадийно храним СЖАТЫЕ
// байты — микшер стримит-декодит их per-voice. CPU-only (warm_and_hot_same): cold->warm = чтение
// файла через модуль. Звуковая система НЕ хранит ресурсы: принимает указатель (demiurg handle) в
// сообщении и читает данные через view() (resource2 = id+type+span+метаданные). span данных живёт,
// пока ресурс не выгружен (координация выгрузки с проигрыванием — follow-up). Короткие звуки
// декодируются в load_cold целиком в PCM (микшер играет их без per-frame декода, PCM-ветка make_decoder).
class sound_resource : public demiurg::resource_interface {
public:
  data_type type = data_type::undefined;
  std::vector<char> data; // сжатые байты ИЛИ (для коротких звуков) сырые PCM-кадры формата sample_format

  // метаданные исходного аудио (заполняются в load_cold ВСЕГДА; см. view()/resource2)
  enum format sample_format = format::unknown;
  uint16_t channels = 0;
  uint32_t sample_rate = 0;
  size_t frames_count = 0;

  sound_resource();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

  resource2 view() const noexcept;
};

} // namespace sound
} // namespace devils_engine

#endif
