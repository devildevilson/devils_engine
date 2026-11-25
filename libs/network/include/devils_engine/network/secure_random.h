#ifndef DEVILS_ENGINE_NETWORK_SECURE_RANDOM_H
#define DEVILS_ENGINE_NETWORK_SECURE_RANDOM_H

#include <cstddef>
#include <cstdint>
#include <span>

// НЕПРЕДСКАЗУЕМЫЕ БАЙТЫ (network_gns) для nonce рукопожатия, идентификаторов сессий и ключей
// билетов повторного входа.
//
// Нейтральный `network` случайности не делает: её приносит вызывающий. Здесь она живёт потому, что
// CSPRNG OpenSSL (`RAND_bytes`) уже в процессе через GNS. Детерминированный генератор, засеянный
// 64 битами (`random_device` + splitmix и т.п.), даёт 256-битному nonce всего 64 бита
// неопределённости: энтропию создаёт источник, а не перемешивание. Причинному состоянию эти байты
// не принадлежат и на корень не влияют.
//
// Отказ источника — критическая ошибка (utils::error): продолжать с предсказуемым nonce значит
// молча ослабить то, ради чего он существует.

namespace devils_engine::network {

void secure_random_bytes(std::span<std::byte> output);

// Ненулевое 64-битное значение: 0 у потребителей зарезервирован под «нет» (сессии, ключа).
[[nodiscard]] uint64_t secure_random_nonzero_u64();

} // namespace devils_engine::network

#endif
