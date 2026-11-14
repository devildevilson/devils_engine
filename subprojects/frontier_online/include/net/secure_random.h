#ifndef FRONTIER_ONLINE_NET_SECURE_RANDOM_H
#define FRONTIER_ONLINE_NET_SECURE_RANDOM_H

#include <cstddef>
#include <cstdint>
#include <span>

// НЕПРЕДСКАЗУЕМЫЕ БАЙТЫ для nonce рукопожатия и идентификатора сессии.
//
// Источник — CSPRNG OpenSSL (`RAND_bytes`), который уже в процессе через GNS. Детерминированный
// генератор (mt19937 и т.п.), засеянный 64 битами, давал бы 256-битному nonce всего 64 бита
// неопределённости, и никакой прогрев или перемешивание их не добавят: энтропию создаёт
// источник, а не миксер. Причинному состоянию эти байты не принадлежат и на корень не влияют.
//
// Отказ источника — критическая ошибка (utils::error): продолжать рукопожатие с предсказуемым
// nonce значит молча ослабить то, ради чего nonce существует.

namespace frontier_online::net {

void secure_random_bytes(std::span<std::byte> output);

// Ненулевое 64-битное значение: 0 зарезервирован под «сессии нет».
[[nodiscard]] uint64_t secure_random_nonzero_u64();

} // namespace frontier_online::net

#endif
