#ifndef DEVILS_ENGINE_AESTHETICS_SINK_H
#define DEVILS_ENGINE_AESTHETICS_SINK_H

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "devils_engine/utils/serialization_sink.h"
#include "serialization.h"                   // world, writer/reader, dump_world/load_world

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

using utils::serial::container_magic;
using utils::serial::container_version;
using utils::serial::disk_policy;
using utils::serial::network_policy;
using utils::serial::seal;
using utils::serial::sink_policy;
using utils::serial::unseal;

// --- удобные обёртки для одиночного мира (payload = ровно один dump_world) ---------
std::vector<std::byte> pack(const world* w, const sink_policy& policy = disk_policy,
                            std::span<const uint8_t> screenshot = {});
bool unpack(std::span<const std::byte> data, world* w,
            std::vector<uint8_t>* screenshot_out = nullptr);

bool save_to_file(const world* w, const std::string& path, const sink_policy& policy = disk_policy,
                  std::span<const uint8_t> screenshot = {});
bool load_from_file(world* w, const std::string& path, std::vector<uint8_t>* screenshot_out = nullptr);

} // namespace serial
} // namespace aesthetics
} // namespace devils_engine

#endif
