#ifndef DEVILS_ENGINE_NETWORK_GNS_RUNTIME_H
#define DEVILS_ENGINE_NETWORK_GNS_RUNTIME_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string_view>

#include <steam/steamnetworkingtypes.h>

#include "gns_transport.h"

// Процессная обвязка GNS для потребителей адаптера (network_gns).
//
// GNS — глобальный объект процесса: повторный Init ничего не делает, а первый же Kill гасит сеть
// всем. Поэтому `gns_runtime` считает владельцев: первый поднимает, последний гасит, и рантаймов в
// процессе может быть несколько (клиент рядом с авторитетом в одном тесте). Он же владеет диспетчером.

namespace devils_engine::network {

class gns_runtime {
public:
  // endpoints — сколько транспортов будет у диспетчера. Отказ Init — utils::error.
  explicit gns_runtime(size_t endpoints);
  ~gns_runtime();
  gns_runtime(const gns_runtime&) = delete;
  gns_runtime& operator=(const gns_runtime&) = delete;

  gns_dispatcher& dispatcher() noexcept {
    return *dispatcher_;
  }
  void pump() {
    dispatcher_->pump();
  }

private:
  std::unique_ptr<gns_dispatcher> dispatcher_;
};

// У обособленного GNS нет настроенного удостоверяющего центра: сессия «без аутентификации»
// объявляется явно (IP_AllowWithoutAuth = 2), чтобы шифрование пакетов не принимали за личность.
std::array<SteamNetworkingConfigValue_t, 1> gns_unauthenticated_options() noexcept;

struct gns_listen_outcome {
  gns_status status = gns_status::ok;
  gns_listener listener;
  uint16_t port = 0;
  bool ephemeral = false; // порт выдала ОС
};

// Слушать объявленный порт. Запасного пути нет намеренно: оператор, назвавший порт и молча
// получивший другой, направит ведомых не туда.
gns_listen_outcome gns_listen_on(gns_transport& transport, uint32_t host, uint16_t port);

// Слушать порт, выданный ОС. Закреплённая сборка GNS нулевой порт не принимает — это свойство
// бэкенда, поэтому запасной путь — перебор [first_port, first_port + port_count): записанный, а не
// спрятанный. backend_rejected на порту — занят, идём дальше; другой отказ — сразу наружу.
gns_listen_outcome gns_listen_any(gns_transport& transport, uint32_t host, uint16_t first_port,
                                  uint16_t port_count);

// Исход попытки отправки для потребителя: отправлено, нет такого соединения, давление (подождать),
// отказ.
enum class gns_send_outcome : uint8_t { sent, no_peer, backpressure, refused };
gns_send_outcome classify_send(gns_status status) noexcept;

// Имя статуса для журнала.
std::string_view describe(gns_status status) noexcept;

} // namespace devils_engine::network

#endif
