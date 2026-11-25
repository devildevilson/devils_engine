#ifndef FRONTIER_ONLINE_NET_PROTOCOL_H
#define FRONTIER_ONLINE_NET_PROTOCOL_H

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include <devils_engine/network/credential.h>
#include <devils_engine/network/document_chunks.h>
#include <devils_engine/network/session_wire.h>

#include "core/causal_content.h"
#include "net/canonical_action.h"

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
inline constexpr uint32_t protocol_version = 5;

// Числовой профиль. Пока это «родной float, профиль не зафиксирован»: NUM-01..04 ещё не пройдены,
// поэтому совпадение поля означает только, что обе стороны ЗАЯВИЛИ одно и то же, и не является
// обещанием побитового совпадения арифметики на разных машинах. Когда появится строгий профиль,
// изменится это число, и старые сборки будут отвергнуты полем, которое уже проверяется.
inline constexpr uint32_t numeric_profile = 1;

[[nodiscard]] network::session_compatibility local_compatibility(const core::causal_content& content);

// ─────────────────────────────────────────────────────────────────────────────────────────────
// Проектные сообщения.
// ─────────────────────────────────────────────────────────────────────────────────────────────

enum class message : uint8_t {
  world_declaration = 1, // авторитет -> клиент, управляющая полоса
  checkpoint_begin = 2,  // авторитет -> клиент, объёмная полоса
  checkpoint_chunk = 3,  // авторитет -> клиент, объёмная полоса
  join_report = 4,       // клиент -> авторитет, управляющая полоса
  join_confirmed = 5,    // авторитет -> клиент, live mode: отчёт принят
  live_caught_up = 6,    // авторитет -> клиент: все bundles до этого тика уже в control lane
  session_terminated = 7, // авторитет -> клиент, управляющая полоса: почему обслуживание прекращено
  reconnect_grant = 8,    // авторитет -> клиент, live mode: ключ повторного входа
  resume_plan = 9,        // авторитет -> клиент, после рукопожатия с продолжением: откуда догонять
  leave = 10,             // клиент -> авторитет, live mode: уходит сам (слот освобождается сразу)
};

// Budget for the complete causal document, not for one GNS message. The
// receiver checks it before allocating from an untrusted total_bytes field.
inline constexpr uint32_t max_checkpoint_bytes = 16 * 1024 * 1024;

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

// Формат куска и сборка документа по порядку — network/document_chunks.h.
using chunk_view = devils_engine::network::document_chunk_view;

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
  // Объявление мира не разобралось. Отдельно от `generator_missing`: «генератора нет» и «сообщение
  // битое» — разные беды, и отчёт не должен выдавать одну за другую.
  world_declaration_malformed = 7,
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

// Only after this control-lane message may the joined client emit hot intents.
// It binds the authority's agreement to the same membership and checkpoint.
struct join_confirmation {
  uint64_t session = 0;
  uint64_t logical_peer = 0;
  uint64_t tick = 0;
  uint64_t root = 0;
};

[[nodiscard]] bool encode(const join_confirmation& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, join_confirmation& out);

struct catchup_marker {
  uint64_t session = 0;
  uint64_t logical_peer = 0;
  uint64_t tick = 0;
};

[[nodiscard]] bool encode(const catchup_marker& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, catchup_marker& out);

// КЛЮЧ ПОВТОРНОГО ВХОДА. После подтверждения присоединения (и после каждого продолжения) авторитет
// выдаёт участнику билет на ЕГО сессию (у каждого участника своя) и выведенный из неё секрет
// (`network/credential.h`); соединение GNS шифровано, секрет едет один раз. Билет переиздаётся до
// истечения: долгая сессия не держится на одном билете. `hold_ms` — сколько авторитет держит слот
// после обрыва: столько клиент и пытается вернуться тем же игроком.
struct reconnect_grant {
  uint64_t session = 0;
  uint64_t logical_peer = 0;
  network::reconnect_credential credential;
  network::credential_mac secret{};
  uint32_t hold_ms = 0;
};

// Издатель билетов frontier и MAC-политика `network/credential.h`: HMAC-SHA256 (utils/hmac.h).
// Ключ авторитета — случайный на процесс (перезапуск сервера делает все билеты чужими: это новый
// вход). У клиента ключа нет: ему нужен только `keyed_mac` выведенного секрета.
inline constexpr uint32_t credential_issuer = UINT32_C(0x31524f46); // "FOR1"

struct credential_policy {
  const network::credential_mac* authority_key = nullptr;
  [[nodiscard]] bool authority_mac(uint32_t issuer, std::span<const std::byte> message,
                                   network::credential_mac& out) const;
  [[nodiscard]] bool keyed_mac(const network::credential_mac& key, std::span<const std::byte> message,
                               network::credential_mac& out) const;
};

[[nodiscard]] bool encode(const reconnect_grant& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, reconnect_grant& out);

// ПЛАН ПРОДОЛЖЕНИЯ — первое проектное сообщение после рукопожатия с продолжением, вместо объявления
// мира (мир у клиента уже есть). `journal`: клиент стоит на своём подтверждённом тике (`tick`/`root`
// — его же якорь, авторитет нашёл этот корень в своей истории), дальше идут тики tick+1..N.
// `checkpoint`: по объёмной полосе едет снимок тика `tick` из кольца авторитета, затем тики
// tick+1..N. В обоих случаях клиент отвечает обычным `join_report` и ждёт `join_confirmed`.
enum class resume_mode : uint8_t {
  journal = 1,
  checkpoint = 2,
};

struct resume_plan {
  uint64_t session = 0;
  uint64_t logical_peer = 0;
  resume_mode mode = resume_mode::journal;
  uint64_t tick = 0;
  uint64_t root = 0;
};

[[nodiscard]] bool encode(const resume_plan& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, resume_plan& out);

// Корень frontier — u64 (murmur64 по разделам состояния), а поле якоря в рукопожатии — 32-байтный
// digest. Корень едет в первых 8 байтах (little-endian), остальные — нули; декодер требует нули.
[[nodiscard]] devils_engine::utils::digest anchor_digest(uint64_t root) noexcept;
[[nodiscard]] bool anchor_root(const devils_engine::utils::digest& digest, uint64_t& root) noexcept;

// УХОД. Добровольный уход отличается от обрыва только словом клиента: закрытое соединение без него —
// обрыв, и слот держится окно удержания. Сообщение — один байт типа; авторитет отвечает закрытием.
[[nodiscard]] bool encode_leave(std::vector<std::byte>& out);
[[nodiscard]] bool is_leave(std::span<const std::byte> bytes) noexcept;

// Payload канонической записи (`canonical_food_action`) — в "net/canonical_action.h".

// Почему авторитет прекратил обслуживать участника ПОСЛЕ рукопожатия. Отказ рукопожатия называет
// себя `session_refusal_reason`; всё, что случилось позже, — это. Нумерация стабильна: значение
// поедет по проводу в `session_terminated`.
enum class termination_reason : uint8_t {
  violations = 1,        // превышен порог нарушений протокола
  timeout = 2,           // фаза не завершилась в объявленный срок
  history_exhausted = 3, // участник отстал за начало удерживаемой истории
  shutdown = 4,          // авторитет завершает работу
  disagreement = 5,      // отчёт о присоединении не совпал с отправленным состоянием
};

inline constexpr uint8_t termination_reason_max = uint8_t(termination_reason::disagreement);

// ЗАВЕРШЕНИЕ СЕССИИ. Последнее слово авторитета перед закрытием соединения: отказ обязан
// назваться и после рукопожатия. Шлётся только установленному участнику (до рукопожатия у него
// нет грамматики проектных сообщений) и по управляющей полосе, после всего уже поставленного.
struct session_terminated {
  termination_reason reason = termination_reason::violations;
};

[[nodiscard]] bool encode(const session_terminated& value, std::vector<std::byte>& out);
[[nodiscard]] bool decode(std::span<const std::byte> bytes, session_terminated& out);

[[nodiscard]] const char* describe(join_status status) noexcept;
[[nodiscard]] const char* describe(termination_reason reason) noexcept;
[[nodiscard]] const char* describe(network::session_refusal_reason reason) noexcept;

} // namespace frontier_online::net

#endif
