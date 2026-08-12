#ifndef DEVILS_ENGINE_SOUND_SOUND_RESOURCE_H
#define DEVILS_ENGINE_SOUND_SOUND_RESOURCE_H

#include <atomic>
#include <cstdint>
#include <memory>
#include <span>

#include "devils_engine/demiurg/resource_base.h"
#include "resource.h" // resource2 + data_type (через common.h)

namespace devils_engine {
namespace sound {

// Звук как demiurg-ресурс: управляется потоком ассетов (как mesh/texture). Стадийно храним СЖАТЫЕ
// байты — микшер стримит-декодит их per-voice. CPU-only (warm_and_hot_same): cold->warm = чтение
// файла через модуль. Опубликованные байты лежат в immutable resource_blob: broker-команда берёт
// shared pin до отправки, task сохраняет его до терминального удаления. unload снимает только ссылку
// ресурса, поэтому queued/active playback безопасно доигрывает старое поколение. Короткие звуки
// декодируются в load_cold целиком в PCM (микшер играет их без per-frame декода).
class sound_resource : public demiurg::resource_interface {
public:
  sound_resource();

  void load_cold(const utils::safe_handle_t& handle) override;
  void load_warm(const utils::safe_handle_t& handle) override;
  void unload_hot(const utils::safe_handle_t& handle) override;
  void unload_warm(const utils::safe_handle_t& handle) override;

  std::shared_ptr<const resource_blob> pin() const noexcept;
  resource2 view() const noexcept;

private:
  std::atomic<std::shared_ptr<const resource_blob>> storage_;
};

} // namespace sound
} // namespace devils_engine

#endif
