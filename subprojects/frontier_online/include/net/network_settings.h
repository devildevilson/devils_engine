#ifndef FRONTIER_ONLINE_NET_NETWORK_SETTINGS_H
#define FRONTIER_ONLINE_NET_NETWORK_SETTINGS_H

#include <cstdint>
#include <string_view>

#include <devils_engine/network/reconnect.h>

// Сетевые сроки frontier_online — явно и в одном месте. Значения ниже — умолчания (их же берут
// authority_config и follower_config); файл `network.tavl` в папке проекта (у клиента — корень
// продукта, у сервера — `server/`) может переопределить любое поле. Файла нет — умолчания; процесс
// ничего не пишет на диск (headless-сервер должен жить и на файловой системе только для чтения).
// Все сроки — миллисекунды; 0 у срока фазы — «без срока».

namespace frontier_online::net {

struct network_settings {
  // Ведомый: сроки фаз входа и тишина живой сессии.
  struct follower_timeouts {
    uint32_t connect_ms = 5000;
    uint32_t handshake_ms = 5000;
    uint32_t world_ms = 5000;
    uint32_t transfer_stall_ms = 5000;
    uint32_t confirmation_ms = 5000;
    uint32_t live_silence_ms = 5000;
  };
  // Авторитет: сроки фаз рукопожатия, отчёта и задержка закрытия после последнего слова.
  struct authority_timeouts {
    uint32_t hello_ms = 2000;
    uint32_t response_ms = 2000;
    uint32_t report_ms = 5000;
    uint32_t report_min_bytes_per_second = 64 * 1024; // срок отчёта растёт с размером состояния
    uint32_t close_linger_ms = 500;
    // Повторный вход с продолжением: сколько слот участника держится после обрыва (его игрок стоит
    // в мире без вводов и занимает место), и срок билета; билет переиздаётся на половине срока.
    uint32_t hold_ms = 30000;
    uint32_t ticket_ms = 600000;
  };
  // Повторный вход графического клиента (network::reconnect_coordinator).
  struct reconnect_timeouts {
    uint32_t suspect_ms = 1000;   // без нового тика: показать
    uint32_t lost_ms = 6000;      // дольше тишины ведомого: он точнее
    uint32_t first_backoff_ms = 500;
    uint32_t max_backoff_ms = 8000;
    uint32_t max_attempts = 8;
  };

  follower_timeouts follower;
  authority_timeouts authority;
  reconnect_timeouts reconnect;
};

inline constexpr network_settings default_network_settings{};
inline constexpr std::string_view network_settings_file = "network.tavl";

// `network.tavl` из папки проекта (utils::project_folder()). Нет файла — умолчания. Файл не
// разбирается или политика повторного входа противоречива — критическая ошибка: это собственная
// конфигурация процесса, а не данные другого узла.
[[nodiscard]] network_settings load_network_settings();

[[nodiscard]] constexpr devils_engine::network::reconnect_policy reconnect_policy_of(
  const network_settings& settings) noexcept {
  return devils_engine::network::reconnect_policy{
    .suspect_after = settings.reconnect.suspect_ms,
    .lost_after = settings.reconnect.lost_ms,
    .first_backoff = settings.reconnect.first_backoff_ms,
    .max_backoff = settings.reconnect.max_backoff_ms,
    .max_attempts = settings.reconnect.max_attempts};
}
static_assert(reconnect_policy_of(default_network_settings).valid());

} // namespace frontier_online::net

#endif
