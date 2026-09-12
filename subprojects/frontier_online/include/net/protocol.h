#ifndef FRONTIER_ONLINE_NET_PROTOCOL_H
#define FRONTIER_ONLINE_NET_PROTOCOL_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <devils_engine/network/session_wire.h>

#include "core/causal_content.h"

// СОБСТВЕННЫЕ КЛАССЫ СООБЩЕНИЙ СТЕНДА — то, чего нет в библиотеке, потому что оно проектное.
// Рукопожатие целиком берётся из `network/session_wire.h`: второй, «свой» netcode рядом с уже
// доказанным — это ровно то, чего запрещает делать TF-NET-01.
//
// РАЗДЕЛЕНИЕ БЕЗ ПРИЗНАКА В БАЙТАХ. Сообщения рукопожатия и проектные сообщения идут по одной
// надёжно-упорядоченной полосе, и различаются они ФАЗОЙ, а не первым байтом: пока рукопожатие не
// завершено, всё пришедшее по управляющей полосе принадлежит ему; после — не принадлежит никогда,
// потому что рукопожатие терминально и назад не возвращается. Упорядоченность полосы делает это
// однозначным.

namespace frontier_online::net {

namespace network = devils_engine::network;

// Версия протокола стенда. Растёт при ЛЮБОМ изменении байтов ниже.
inline constexpr uint32_t protocol_version = 1;

// Числовой профиль. Пока это «родной float, профиль не зафиксирован»: NUM-01..04 ещё не пройдены,
// поэтому совпадение поля означает только, что обе стороны ЗАЯВИЛИ одно и то же, и не является
// обещанием побитового совпадения арифметики на разных машинах. Когда появится строгий профиль,
// изменится это число, и старые сборки будут отвергнуты полем, которое уже проверяется.
inline constexpr uint32_t numeric_profile = 1;

// Интентов на проводе ещё нет — их слой следующий срез. Ноль здесь не заглушка, а объявление
// пустого набора: поле уже сравнивается, и в тот день, когда интенты появятся, несовпадение
// схемы станет отказом само, без правки рукопожатия.
inline constexpr uint32_t intent_schema_fingerprint = 0;

[[nodiscard]] network::session_compatibility local_compatibility(const core::causal_content& content);

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Проектные сообщения.
// ─────────────────────────────────────────────────────────────────────────────────────────────

enum class message : uint8_t {
  world_declaration = 1, // авторитет -> клиент, управляющая полоса
  checkpoint_begin = 2,  // авторитет -> клиент, объёмная полоса
  checkpoint_chunk = 3,  // авторитет -> клиент, объёмная полоса
  join_report = 4,       // клиент -> авторитет, управляющая полоса
};

// Тип первого байта, или 0 для пустого/непонятного сообщения.
[[nodiscard]] uint8_t peek_message_type(std::span<const std::byte> bytes) noexcept;

// ОБЪЯВЛЕНИЕ МИРА. Землю по сети не гоняем — она чистая функция (зерно, ключ), — поэтому едет
// только то, что нужно, чтобы её ПОСЧИТАТЬ, и то, чем потом проверяется, что посчиталось
// одинаково.
//
// Мир принадлежит авторитету: клиент не сверяет своё зерно с чужим, а принимает объявленное.
// Сверяется РЕЗУЛЬТАТ: отпечаток генератора и корень пробного чанка. Первый ловит другой конфиг,
// второй — другую сборку originator при том же конфиге, а это две разные беды.
struct world_declaration {
  uint64_t world_seed = 0;
  uint32_t chunk_size = 0;
  uint64_t generator_fingerprint = 0;
  uint64_t probe_root = 0; // murmur64 по кодам рельефа чанка (0,0)
  std::string generator;   // demiurg-id точки входа
};

// Корень пробного чанка: murmur64 по кодам рельефа чанка (0,0).
//
// Отпечаток генератора считается по ТЕКСТАМ конфига, и он ловит другой конфиг. Эта величина ловит
// другое: тот же конфиг, посчитанный другой сборкой originator. Один чанк — потому что расхождение
// такого рода не бывает редким: если шум считается иначе, он считается иначе везде.
[[nodiscard]] uint64_t terrain_probe_root(core::terrain_source& source);

// Объявление мира по живому источнику земли. Одна функция на обе стороны: авторитет ею объявляет,
// клиент — проверяет, и обе величины приходят из одного кода.
[[nodiscard]] world_declaration declare_world(core::terrain_source& source,
                                              const std::string& generator);

[[nodiscard]] bool encode(const world_declaration& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, world_declaration& out);

// Начало передачи чек-пойнта. Идёт ПО ТОЙ ЖЕ полосе, что и куски: между полосами порядка нет, и
// кусок, обогнавший своё начало, был бы протокольной ошибкой на ровном месте.
struct checkpoint_begin {
  uint64_t tick = 0;
  uint64_t root = 0;
  uint32_t total_bytes = 0;
  uint32_t schema_fingerprint = 0;
};

[[nodiscard]] bool encode(const checkpoint_begin& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, checkpoint_begin& out);

// Единственный класс, несущий собственную длину: он режет то, чей размер не является размером
// сообщения.
[[nodiscard]] bool encode_chunk(uint32_t offset, std::span<const std::byte> payload,
                                std::vector<std::byte>& out);

struct chunk_view {
  uint32_t offset = 0;
  std::span<const std::byte> payload;
};

[[nodiscard]] bool decode_chunk(std::span<const std::byte> bytes, chunk_view& out);

// ОТЧЁТ О ПРИСОЕДИНЕНИИ. Клиент говорит, чем кончилось: каким корнем обернулась загрузка и совпал
// ли мир. Нужен затем, чтобы сравнение двух процессов делал АВТОРИТЕТ, а не человек, который
// смотрит в два окна и сличает числа глазами.
enum class join_status : uint8_t {
  loaded = 0,               // чек-пойнт загружен, корень совпал
  checkpoint_refused = 1,   // документ отвергнут загрузчиком
  root_mismatch = 2,        // загрузился, но корень другой
  world_fingerprint_mismatch = 3, // клиент не может посчитать объявленный мир
  world_probe_mismatch = 4, // считает, но получает другую землю
  generator_missing = 5,    // объявленного генератора у клиента нет
  // Отказ случился на рукопожатии, до единого байта состояния. Отдельное значение, а не
  // умолчание: «загрузчик отверг документ» и «документа никто не присылал» — разные вещи, и
  // печатать вторую как первую значит врать в отчёте.
  handshake_refused = 6,
};

struct join_report {
  join_status status = join_status::checkpoint_refused;
  uint64_t tick = 0;
  uint64_t root = 0;
  uint64_t world_fingerprint = 0;
  uint64_t probe_root = 0;
};

[[nodiscard]] bool encode(const join_report& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, join_report& out);

[[nodiscard]] const char* describe(join_status status) noexcept;
[[nodiscard]] const char* describe(network::session_refusal_reason reason) noexcept;

} // namespace frontier_online::net

#endif
