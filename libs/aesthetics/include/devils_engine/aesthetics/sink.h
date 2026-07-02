#ifndef DEVILS_ENGINE_AESTHETICS_SINK_H
#define DEVILS_ENGINE_AESTHETICS_SINK_H

#include <cstddef>
#include <cstdint>
#include <vector>
#include <span>
#include <string>

#include "devils_engine/utils/compression.h" // compression_level
#include "serialization.h"                    // world, writer/reader, dump_world/load_world

// Sink = ВЕРХНИЙ слой сериализации: тупое ядро (serialize<T>) пишет агрегаты в writer-буфер,
// обёртки-дамперы (dump_world + сторонние структуры) складывают свои данные в ОДИН payload,
// а seal/unseal заворачивают этот готовый payload в пакет (заголовок + checksum + компрессия +
// опц. скриншот). Слои независимы: seal НЕ знает, что внутри payload — мир, сеть-дельта, что угодно.
//
//   vector<byte> ← dump_world ← dump_<side> ← … (собираем payload одним writer'ом)
//   payload → seal → пакет (на диск/в сеть);  пакет → unseal → payload → load_world/load_<side>
//
// Контейнер (обёртка вокруг СОБРАННОГО payload, всё LE):
//   [magic u32][version u16][algo u8][flags u8: bit0=screenshot bit1=compressed]
//   [raw_size u64][payload_size u64][checksum u64]   checksum = murmur64 по СЫРОМУ payload
//   [screenshot_size u32][screenshot bytes]          только если flags&screenshot
//   [payload]                                        density(raw) либо сырьё, если не сжалось
//
// diff disk↔network = только уровень компрессии + скриншот. checksum сверяется по сырым
// байтам, поэтому детерминированность (сортировка мап в serialize) обязательна для сети.

namespace devils_engine {
namespace aesthetics {
namespace serial {

constexpr uint32_t container_magic   = UINT32_C(0xDE5AC001); // 'DE' snapshot container v01
constexpr uint16_t container_version = 1;

struct sink_policy {
  utils::compression_level level;
  bool embed_screenshot;
};
// на замерах zstd: level>1 почти не улучшает ratio, а best(19) даёт +~20% ценой ~x200 времени
// (100k: fast ~14мс vs best ~3.2с). Поэтому дефолты умеренные; best — осознанный knob для архива/async.
inline constexpr sink_policy disk_policy{ utils::compression_level::normal, true };    // zstd 3 + скриншот
inline constexpr sink_policy network_policy{ utils::compression_level::fast, false };  // zstd 1

// --- packet-обёртка (работает над УЖЕ собранным payload, не знает про world) ------
// seal: сырой payload -> сжатый контейнер с checksum (+ опц. скриншот). screenshot — уже готовые
// байты (PNG и т.п.); sink не знает как его снять — это забота render-слоя.
std::vector<uint8_t> seal(std::span<const std::byte> payload, const sink_policy& policy = disk_policy,
                          std::span<const uint8_t> screenshot = {});

// unseal: контейнер -> сырой payload (в raw_out, для чтения дамперами через reader). Валидирует
// magic/версию/checksum/декомпрессию. screenshot_out (если задан) получит встроенный скриншот.
// false при битом контейнере — guard, не throw. Схему payload (fingerprint) стережёт уже load_world.
bool unseal(std::span<const uint8_t> data, std::vector<std::byte>& raw_out,
            std::vector<uint8_t>* screenshot_out = nullptr);

// --- удобные обёртки для одиночного мира (payload = ровно один dump_world) ---------
std::vector<uint8_t> pack(const world* w, const sink_policy& policy = disk_policy,
                          std::span<const uint8_t> screenshot = {});
bool unpack(std::span<const uint8_t> data, world* w, std::vector<uint8_t>* screenshot_out = nullptr);

bool save_to_file(const world* w, const std::string& path, const sink_policy& policy = disk_policy,
                  std::span<const uint8_t> screenshot = {});
bool load_from_file(world* w, const std::string& path, std::vector<uint8_t>* screenshot_out = nullptr);

} // namespace serial
} // namespace aesthetics
} // namespace devils_engine

#endif
