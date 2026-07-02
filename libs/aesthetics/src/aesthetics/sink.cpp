#include "devils_engine/aesthetics/sink.h"

#include <cstddef>
#include <string_view>
#include <fstream>
#include <iterator>

#include "devils_engine/utils/type_traits.h" // utils::murmur_hash64A
#include "devils_engine/utils/core.h"         // utils::warn

namespace devils_engine {
namespace aesthetics {
namespace serial {

namespace {
constexpr uint8_t flag_screenshot = 0x1;
constexpr uint8_t flag_compressed = 0x2;

struct le_writer {
  std::vector<uint8_t>& b;
  void u8(const uint8_t v) { b.push_back(v); }
  void u16(const uint16_t v) { for (int i = 0; i < 2; ++i) u8(uint8_t(v >> (8 * i))); }
  void u32(const uint32_t v) { for (int i = 0; i < 4; ++i) u8(uint8_t(v >> (8 * i))); }
  void u64(const uint64_t v) { for (int i = 0; i < 8; ++i) u8(uint8_t(v >> (8 * i))); }
  void raw(const uint8_t* p, const size_t n) { b.insert(b.end(), p, p + n); }
};

struct le_reader {
  std::span<const uint8_t> b;
  size_t pos = 0;
  bool bad = false;
  bool can(const size_t n) const noexcept { return pos + n <= b.size(); }
  uint8_t  u8()  { if (!can(1)) { bad = true; return 0; } return b[pos++]; }
  uint16_t u16() { uint16_t v = 0; for (int i = 0; i < 2; ++i) v |= uint16_t(u8()) << (8 * i); return v; }
  uint32_t u32() { uint32_t v = 0; for (int i = 0; i < 4; ++i) v |= uint32_t(u8()) << (8 * i); return v; }
  uint64_t u64() { uint64_t v = 0; for (int i = 0; i < 8; ++i) v |= uint64_t(u8()) << (8 * i); return v; }
  std::span<const uint8_t> take(const size_t n) { if (!can(n)) { bad = true; return {}; } auto s = b.subspan(pos, n); pos += n; return s; }
};

uint64_t checksum_of(const std::byte* p, const size_t n) noexcept {
  return utils::murmur_hash64A(std::string_view(reinterpret_cast<const char*>(p), n));
}
} // namespace

std::vector<uint8_t> seal(const std::span<const std::byte> payload_raw, const sink_policy& policy, const std::span<const uint8_t> screenshot) {
  const uint64_t raw_size = payload_raw.size();
  const uint64_t checksum = checksum_of(payload_raw.data(), raw_size);

  const auto* rp = reinterpret_cast<const uint8_t*>(payload_raw.data());
  const std::vector<uint8_t> raw_u8(rp, rp + raw_size);
  const std::vector<uint8_t> compressed = utils::compress(raw_u8, policy.level);

  // фолбэк на сырьё, если density не сжал (мелкий/несжимаемый вход) или дал больше — density
  // требует минимальный выходной буфер и на крошечных данных возвращает пусто; храним как есть.
  const bool use_compressed = !compressed.empty() && compressed.size() < raw_size;
  const std::vector<uint8_t>& payload = use_compressed ? compressed : raw_u8;

  const bool with_shot = policy.embed_screenshot && !screenshot.empty();
  std::vector<uint8_t> bytes;
  bytes.reserve(32 + payload.size() + (with_shot ? screenshot.size() + 4 : 0));

  le_writer wr{bytes};
  wr.u32(container_magic);
  wr.u16(container_version);
  wr.u8(uint8_t(policy.level));
  wr.u8(uint8_t((with_shot ? flag_screenshot : 0) | (use_compressed ? flag_compressed : 0)));
  wr.u64(raw_size);
  wr.u64(payload.size());
  wr.u64(checksum);
  if (with_shot) { wr.u32(uint32_t(screenshot.size())); wr.raw(screenshot.data(), screenshot.size()); }
  wr.raw(payload.data(), payload.size());
  return bytes;
}

bool unseal(const std::span<const uint8_t> data, std::vector<std::byte>& raw_out, std::vector<uint8_t>* screenshot_out) {
  le_reader rd{data};
  const uint32_t magic = rd.u32();
  const uint16_t version = rd.u16();
  rd.u8(); // algo (информ.; density-поток самоописывающийся при decompress)
  const uint8_t flags = rd.u8();
  const uint64_t raw_size = rd.u64();
  const uint64_t payload_size = rd.u64();
  const uint64_t checksum = rd.u64();
  if (rd.bad || magic != container_magic) { utils::warn("snapshot container: bad magic 0x{:08x}", magic); return false; }
  if (version != container_version) { utils::warn("snapshot container: version {} != {}", version, container_version); return false; }

  if (flags & flag_screenshot) {
    const uint32_t ss = rd.u32();
    const auto shot = rd.take(ss);
    if (rd.bad) { utils::warn("snapshot container: truncated screenshot"); return false; }
    if (screenshot_out != nullptr) screenshot_out->assign(shot.begin(), shot.end());
  }

  const auto payload = rd.take(payload_size);
  if (rd.bad) { utils::warn("snapshot container: truncated payload"); return false; }

  std::vector<uint8_t> raw;
  if (flags & flag_compressed) {
    raw.resize(utils::decompress_safe_size(raw_size));
    const size_t n = utils::decompress(payload.data(), payload.size(), raw.data(), raw.size());
    if (n == SIZE_MAX || n != raw_size) { utils::warn("snapshot container: decompress failed ({} vs {})", n, raw_size); return false; }
    raw.resize(n);
  } else {
    if (payload.size() != raw_size) { utils::warn("snapshot container: raw size mismatch"); return false; }
    raw.assign(payload.begin(), payload.end());
  }

  const uint64_t got = checksum_of(reinterpret_cast<const std::byte*>(raw.data()), raw.size());
  if (got != checksum) { utils::warn("snapshot container: checksum mismatch 0x{:016x} vs 0x{:016x}", got, checksum); return false; }

  raw_out.assign(reinterpret_cast<const std::byte*>(raw.data()), reinterpret_cast<const std::byte*>(raw.data()) + raw.size());
  return true;
}

std::vector<uint8_t> pack(const world* w, const sink_policy& policy, const std::span<const uint8_t> screenshot) {
  const std::vector<std::byte> raw = dump_world(w); // payload = ровно один мир
  return seal(raw, policy, screenshot);
}

bool unpack(const std::span<const uint8_t> data, world* w, std::vector<uint8_t>* screenshot_out) {
  std::vector<std::byte> raw;
  if (!unseal(data, raw, screenshot_out)) return false;
  in_t in{raw};
  return load_world(w, in);
}

bool save_to_file(const world* w, const std::string& path, const sink_policy& policy, const std::span<const uint8_t> screenshot) {
  const auto bytes = pack(w, policy, screenshot);
  std::ofstream f(path, std::ios::binary);
  if (!f) { utils::warn("snapshot: cannot open '{}' for write", path); return false; }
  f.write(reinterpret_cast<const char*>(bytes.data()), std::streamsize(bytes.size()));
  return bool(f);
}

bool load_from_file(world* w, const std::string& path, std::vector<uint8_t>* screenshot_out) {
  std::ifstream f(path, std::ios::binary);
  if (!f) { utils::warn("snapshot: cannot open '{}' for read", path); return false; }
  const std::vector<uint8_t> data((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
  return unpack(data, w, screenshot_out);
}

} // namespace serial
} // namespace aesthetics
} // namespace devils_engine
