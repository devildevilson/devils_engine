#ifndef DEVILS_ENGINE_NETLAB01_REPORT_H
#define DEVILS_ENGINE_NETLAB01_REPORT_H

#include "lab.h"
#include "link.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

// The run's record, written to a file so several machines can be compared after
// the fact instead of by reading four terminals.
//
// Two parts, because the questions are of two kinds. Scalars answer "what
// happened" — the state root, the order accounting, the session events — and a
// SAMPLE TABLE answers "what was the link like", which a single reading at the
// end cannot: by then the connection has been idle through the linger and the
// backend reports a stale interval. The table has a declared capacity and a
// declared cadence; a report which grows with the run is an allocation the run
// controls.
//
// The format is deliberately dull: `key = value` lines and one whitespace table.
// It needs no library to write and none to read, and `grep`/`awk` are enough to
// compare two machines' reports, which is the whole point of writing it down.

namespace netlab01 {

struct lab_sample {
  uint64_t tick = 0;
  uint64_t wall_ms = 0;
  int ping_ms = 0;
  float quality_local = 0, quality_remote = 0;
  float in_pps = 0, out_pps = 0;
  int pending_reliable = 0;
};

class lab_report {
public:
  // Every 25 ticks is half a second at the 20 ms pacing a LAN run wants, and
  // 512 rows cover a 12800-tick run before the cadence has to be reconsidered.
  static constexpr uint64_t sample_every_ticks = 25;
  static constexpr size_t sample_capacity = 512;

  lab_report() {
    samples_.reserve(sample_capacity);
  }

  void set(const std::string_view key, const std::string_view value) {
    scalars_.emplace_back(std::string(key), std::string(value));
  }
  void set(const std::string_view key, const uint64_t value) {
    set(key, std::to_string(value));
  }
  void set(const std::string_view key, const double value) {
    char buffer[64];
    std::snprintf(buffer, sizeof(buffer), "%.6g", value);
    set(key, std::string_view(buffer));
  }

  // Dropped rather than grown, and the drop is reported: a truncated table is a
  // fact about the run, not something to hide by reallocating.
  void sample(const lab_sample& value) {
    if (samples_.size() == sample_capacity) {
      ++dropped_samples_;
      return;
    }
    samples_.push_back(value);
  }

  [[nodiscard]] bool due(const uint64_t tick) const noexcept {
    return tick != 0 && tick % sample_every_ticks == 0 && tick != last_sampled_;
  }
  void mark_sampled(const uint64_t tick) noexcept {
    last_sampled_ = tick;
  }

  // Derived from the table so the header answers "what was the link like"
  // without anyone parsing rows.
  void summarize_link() {
    int ping_min = 0, ping_max = 0;
    double ping_sum = 0, quality_sum = 0, quality_min = 0;
    // Two counters, not one. The backend reports a negative reading for "no
    // interval yet", and ping and quality do not become available on the same
    // sample — dividing the quality sum by the ping count diluted the mean with
    // rows it had deliberately skipped, which read as a degraded link on a
    // loopback run that never lost a packet.
    size_t ping_counted = 0, quality_counted = 0;
    for (const auto& row : samples_) {
      if (row.ping_ms >= 0) {
        if (ping_counted == 0) ping_min = ping_max = row.ping_ms;
        ping_min = row.ping_ms < ping_min ? row.ping_ms : ping_min;
        ping_max = row.ping_ms > ping_max ? row.ping_ms : ping_max;
        ping_sum += row.ping_ms;
        ++ping_counted;
      }
      if (row.quality_local >= 0) {
        if (quality_counted == 0) quality_min = row.quality_local;
        quality_sum += row.quality_local;
        quality_min = row.quality_local < quality_min ? row.quality_local : quality_min;
        ++quality_counted;
      }
    }
    set("link.samples", uint64_t(samples_.size()));
    set("link.samples_dropped", dropped_samples_);
    set("link.samples_with_ping", uint64_t(ping_counted));
    set("link.samples_with_quality", uint64_t(quality_counted));
    if (ping_counted == 0) {
      // "No data" is a different answer from "no loss", and the report has to
      // say which one it is.
      set("link.ping_ms", std::string_view("no-data"));
    } else {
      set("link.ping_ms.min", uint64_t(ping_min));
      set("link.ping_ms.max", uint64_t(ping_max));
      set("link.ping_ms.mean", ping_sum / double(ping_counted));
    }
    if (quality_counted == 0) {
      set("link.quality_local", std::string_view("no-data"));
      return;
    }
    set("link.quality_local.min", quality_min);
    set("link.quality_local.mean", quality_sum / double(quality_counted));
  }

  [[nodiscard]] bool write(const std::filesystem::path& path) const {
    std::ofstream out(path, std::ios::trunc);
    if (!out) return false;
    out << "# NET-LAB-01 run report\n";
    for (const auto& [key, value] : scalars_) out << key << " = " << value << '\n';
    out << "\n# tick wall_ms ping_ms quality_local quality_remote in_pps out_pps "
           "pending_reliable\n[samples]\n";
    for (const auto& row : samples_) {
      out << row.tick << ' ' << row.wall_ms << ' ' << row.ping_ms << ' ' << row.quality_local
          << ' ' << row.quality_remote << ' ' << row.in_pps << ' ' << row.out_pps << ' '
          << row.pending_reliable << '\n';
    }
    return bool(out);
  }

private:
  std::vector<std::pair<std::string, std::string>> scalars_;
  std::vector<lab_sample> samples_;
  uint64_t dropped_samples_ = 0;
  uint64_t last_sampled_ = 0;
};

// The build's identity. Two machines disagreeing is the case this exists for,
// and the first question is always whether they were the same build.
inline void describe_build(lab_report& report) {
#if defined(__VERSION__)
  report.set("build.compiler", std::string_view(__VERSION__));
#endif
#if defined(NDEBUG)
  report.set("build.assertions", std::string_view("off"));
#else
  report.set("build.assertions", std::string_view("on"));
#endif
#if defined(__AVX2__)
  report.set("build.isa", std::string_view("avx2"));
#elif defined(__AVX__)
  report.set("build.isa", std::string_view("avx"));
#elif defined(__SSE4_2__)
  report.set("build.isa", std::string_view("sse4.2"));
#else
  report.set("build.isa", std::string_view("baseline"));
#endif
}

// What the two peers must agree on before the first tick. Written out so a
// disagreement can be localized by diffing two reports rather than guessed at.
inline void describe_compatibility(lab_report& report,
                                   const net::session_compatibility& value = lab_compatibility()) {
  report.set("compat.handshake_format", uint64_t(value.handshake_format));
  report.set("compat.protocol_version", uint64_t(value.protocol_version));
  report.set("compat.state_schema_fingerprint", uint64_t(value.state_schema_fingerprint));
  report.set("compat.intent_schema_fingerprint", uint64_t(value.intent_schema_fingerprint));
  report.set("compat.numeric_profile", uint64_t(value.numeric_profile));
  std::string root;
  root.reserve(value.content_root.size() * 2);
  for (const auto byte : value.content_root) {
    char pair[3];
    std::snprintf(pair, sizeof(pair), "%02x", unsigned(byte));
    root += pair;
  }
  report.set("compat.content_root", root);
}

inline void describe_conditions(lab_report& report, const std::string_view prefix,
                                const lab_link::conditions& value) {
  report.set(std::string(prefix) + ".available", uint64_t(value.available ? 1 : 0));
  report.set(std::string(prefix) + ".ping_ms", uint64_t(value.ping_ms < 0 ? 0 : value.ping_ms));
  report.set(std::string(prefix) + ".quality_local", double(value.quality_local));
  report.set(std::string(prefix) + ".in_pps", double(value.in_packets_per_second));
}

} // namespace netlab01

#endif
