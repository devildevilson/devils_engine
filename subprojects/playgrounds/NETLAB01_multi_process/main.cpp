#include "authority.h"
#include "follower.h"
#include "report.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <string_view>
#include <thread>

#if defined(_WIN32)
#  define LAB_POPEN _popen
#  define LAB_PCLOSE _pclose
#  define LAB_EXIT_CODE(status) (status)
#else
#  include <sys/wait.h>
#  define LAB_POPEN popen
#  define LAB_PCLOSE pclose
#  define LAB_EXIT_CODE(status) (WIFEXITED(status) ? WEXITSTATUS(status) : -1)
#endif

// NET-LAB-01: the same session fixture as two real processes over real UDP.
//
// The rendezvous is a FILE, not a fixed port and not a pipe. A fixed port fails
// on a busy machine, a port scan is merely flaky, and a pipe would need
// platform process handles; binding port zero and publishing what the operating
// system gave us has neither problem and is what a real server does anyway.

namespace {
using namespace netlab01;
using namespace std::chrono_literals;

enum class role : uint8_t { verify, authority, follower, intruder };

// A join which must be REFUSED, and refused with a reason. The intruder
// presents a credential that is perfectly valid and already in use, which is
// the only way to reach the capacity branch: an undeclared token would be
// identity_rejected instead, a different answer to a different question.
inline constexpr int lab_intruder_exit = 8;

struct options {
  role which = role::verify;
  std::string scenario = "continuous";
  // Used by the local harness only: peers on other machines cannot read a
  // shared directory, which is exactly why --listen/--connect exist.
  std::filesystem::path rendezvous;
  // Loopback by default. Selects the interface in rendezvous mode.
  std::string address = "127.0.0.1";
  // "A.B.C.D:PORT". Present means a DISTRIBUTED run: the authority binds this
  // exact endpoint and the followers are told it outright.
  std::string endpoint;
  std::filesystem::path ticket;
  // Where the run's record goes. Empty means no file; the report is otherwise
  // written on every exit path, including the failing ones.
  std::filesystem::path report;
  size_t index = 0;
  size_t followers = 0;   // 0 = the scenario's own count
  // Overrides the scenario's run length. A LAN session wants a longer run than
  // a registered test can afford, and the backend's connection-quality window
  // is longer than the test's whole run.
  uint64_t final_tick = 0;
  uint64_t tick_ms = 0;
  uint64_t suspect_ms = 0;
  uint64_t lost_ms = 0;
  // How long a process waits before giving up on the run. The harness spawns
  // everything at once and a minute is plenty; an operator starting processes
  // on several machines by hand needs minutes, so the default differs by mode
  // rather than being one number that is wrong for one of them.
  uint64_t deadline_s = 0;
  uint64_t intent_lead = 0;
  bool resume = false;
  bool quiet = false;
};

struct endpoint_value {
  uint32_t host = 0;
  uint16_t port = 0;
};

std::chrono::seconds wall_deadline(const options& opts) {
  if (opts.deadline_s != 0) return std::chrono::seconds(opts.deadline_s);
  return std::chrono::seconds(opts.endpoint.empty() ? 60 : 900);
}

std::string dotted_quad(const uint32_t host) {
  return std::to_string((host >> 24) & 0xffu) + '.' + std::to_string((host >> 16) & 0xffu) + '.' +
         std::to_string((host >> 8) & 0xffu) + '.' + std::to_string(host & 0xffu);
}

// Dotted quad to the host order GNS wants. A laboratory parser: refusing
// anything it does not understand is the whole of its error handling.
std::optional<uint32_t> parse_ipv4(const std::string_view text) {
  uint32_t value = 0;
  size_t start = 0, parts = 0;
  while (parts < 4) {
    const auto dot = text.find('.', start);
    const auto piece = text.substr(start, dot == std::string_view::npos ? dot : dot - start);
    if (piece.empty() || piece.size() > 3) return std::nullopt;
    unsigned octet = 0;
    for (const char c : piece) {
      if (c < '0' || c > '9') return std::nullopt;
      octet = octet * 10 + unsigned(c - '0');
    }
    if (octet > 255) return std::nullopt;
    value = (value << 8) | octet;
    ++parts;
    if (dot == std::string_view::npos) break;
    start = dot + 1;
  }
  return parts == 4 ? std::optional(value) : std::nullopt;
}

std::optional<uint32_t> parse_ipv4(const std::string_view text);

std::optional<endpoint_value> parse_endpoint(const std::string_view text) {
  const auto colon = text.rfind(':');
  if (colon == std::string_view::npos || colon + 1 == text.size()) return std::nullopt;
  const auto host = parse_ipv4(text.substr(0, colon));
  if (!host) return std::nullopt;
  unsigned port = 0;
  for (const char c : text.substr(colon + 1)) {
    if (c < '0' || c > '9') return std::nullopt;
    port = port * 10 + unsigned(c - '0');
    if (port > 65535) return std::nullopt;
  }
  if (port == 0) return std::nullopt;
  return endpoint_value{*host, uint16_t(port)};
}

lab_schedule schedule_for(const std::string_view scenario) {
  if (scenario == "continuous") return lab_continuous_schedule();
  if (scenario == "killed") return lab_killed_schedule();
  utils::error{}("NET-LAB-01: unknown scenario '{}'", scenario);
  return {};
}

lab_schedule schedule_with(const options& opts) {
  auto schedule = schedule_for(opts.scenario);
  if (opts.final_tick != 0) schedule.final_tick = opts.final_tick;
  if (opts.tick_ms != 0) schedule.tick_period_ms = opts.tick_ms;
  if (opts.suspect_ms != 0) schedule.suspect_after_ms = opts.suspect_ms;
  if (opts.lost_ms != 0) schedule.lost_after_ms = opts.lost_ms;
  // Every count the schedule derives from the roster follows from this one
  // number, because `lab_schedule` asks "does this failure hit anyone" rather
  // than assuming a full roster.
  if (opts.followers != 0) schedule.followers = std::min(opts.followers, lab_max_followers);
  if (opts.intent_lead != 0) schedule.intent_lead_ticks = opts.intent_lead;
  return schedule;
}

std::filesystem::path port_path(const std::filesystem::path& base) {
  return base / "port";
}
std::filesystem::path ticket_path(const std::filesystem::path& base, const size_t index) {
  return base / ("ticket-" + std::to_string(index));
}
std::filesystem::path started_path(const std::filesystem::path& base) {
  return base / "started";
}

// The intruder must arrive AFTER the roster is seated, or it would race for a
// free slot and refuse a legitimate follower instead. The authority publishes
// the moment its clock starts, so the wait is on a fact rather than a sleep.
bool await_started(const std::filesystem::path& base) {
  const auto deadline = std::chrono::steady_clock::now() + 15s;
  while (std::chrono::steady_clock::now() < deadline) {
    if (std::filesystem::exists(started_path(base))) return true;
    std::this_thread::sleep_for(5ms);
  }
  return false;
}

bool publish_port(const std::filesystem::path& base, const uint16_t port) {
  const auto target = port_path(base);
  const auto temporary = std::filesystem::path(target).concat(".tmp");
  {
    std::ofstream out(temporary, std::ios::trunc);
    if (!out) return false;
    out << port << '\n';
    if (!out) return false;
  }
  std::error_code code;
  std::filesystem::rename(temporary, target, code);
  return !code;
}

// Polling a file is the whole rendezvous: the follower has nothing useful to do
// before the authority is listening anyway.
std::optional<uint16_t> await_port(const std::filesystem::path& base) {
  const auto target = port_path(base);
  const auto deadline = std::chrono::steady_clock::now() + 10s;
  while (std::chrono::steady_clock::now() < deadline) {
    std::ifstream in(target);
    unsigned value = 0;
    if (in && (in >> value) && value != 0 && value <= 0xffffu) return uint16_t(value);
    std::this_thread::sleep_for(5ms);
  }
  return std::nullopt;
}

int run_authority(const options& opts) {
  const auto schedule = schedule_with(opts);

  // Two ways to be reachable, and they are not interchangeable. A distributed
  // run names the endpoint outright, because peers on other machines cannot
  // read a rendezvous file; the local harness lets the operating system pick a
  // port and publishes it, because a fixed port fails on a busy machine.
  uint32_t host_address = 0;
  uint16_t declared_port = 0;
  if (!opts.endpoint.empty()) {
    const auto parsed = parse_endpoint(opts.endpoint);
    if (!parsed) {
      std::cerr << "NET-LAB-01 authority: '" << opts.endpoint
                << "' is not A.B.C.D:PORT\n";
      return EXIT_FAILURE;
    }
    host_address = parsed->host;
    declared_port = parsed->port;
  } else {
    const auto parsed = parse_ipv4(opts.address);
    if (!parsed) {
      std::cerr << "NET-LAB-01 authority: '" << opts.address << "' is not a dotted quad\n";
      return EXIT_FAILURE;
    }
    host_address = *parsed;
  }

  authority_run authority(schedule);
  const auto bound = authority.listen(host_address, declared_port);
  if (opts.rendezvous.empty()) {
    // Standalone: the operator needs the line below to point the followers, and
    // needs to be told once that this session is deliberately unauthenticated.
    std::cout << "authority listening " << dotted_quad(host_address) << ':' << bound.port
              << " followers=" << schedule.followers
              << " tick_ms=" << schedule.tick_period_ms
              << " final_tick=" << schedule.final_tick
              << " suspect_ms=" << schedule.suspect_ms()
              << " lost_ms=" << schedule.lost_ms()
              << " auth=none(IP_AllowWithoutAuth)\n";
    std::cout.flush();
  } else if (!publish_port(opts.rendezvous, bound.port)) {
    std::cerr << "NET-LAB-01 authority: cannot publish its port\n";
    return EXIT_FAILURE;
  }

  // The report is filled as the run goes and written on EVERY exit path,
  // including the failing ones: a failed run is when its record is wanted most.
  lab_report report;
  describe_build(report);
  describe_compatibility(report);
  report.set("run.role", std::string_view("authority"));
  report.set("run.scenario", opts.scenario);
  report.set("run.endpoint", dotted_quad(host_address) + ":" + std::to_string(bound.port));
  report.set("run.ephemeral_bind", uint64_t(bound.ephemeral ? 1 : 0));
  report.set("run.followers_expected", uint64_t(schedule.followers));
  report.set("run.tick_period_ms", schedule.tick_period_ms);
  report.set("run.final_tick_scheduled", schedule.final_tick);
  report.set("run.suspect_ms", schedule.suspect_ms());
  report.set("run.lost_ms", schedule.lost_ms());
  report.set("run.checkpoint_every", schedule.checkpoint_every);
  const uint64_t wall_origin = monotonic_ms();

  const auto finish = [&](const std::string_view outcome, const int code) {
    report.set("run.outcome", outcome);
    report.set("run.wall_ms", monotonic_ms() - wall_origin);
    report.set("state.tick", authority.host().state.tick);
    report.set("state.root", lab_root(authority.host()));
    report.set("state.final_tick_announced", authority.final_tick());
    const auto& c = authority.counters();
    report.set("session.admissions", c.admissions);
    report.set("session.resumes_from_hold", c.resumes_from_hold);
    report.set("session.resumes_by_migration", c.resumes_by_migration);
    report.set("session.refusals", c.refusals);
    report.set("session.joins_refused_no_capacity", c.joins_refused_no_capacity);
    report.set("session.recoveries_planned", c.recoveries_planned);
    report.set("session.checkpoint_chunks_sent", c.chunks_sent);
    report.set("session.replay_bundles_sent", c.replay_bundles_sent);
    report.set("bundles.sent", c.bundles_sent);
    report.set("bundles.multi_principal_ticks", c.multi_principal_ticks);
    report.set("copies.accepted", c.intents_accepted);
    report.set("copies.duplicate", c.intents_duplicate);
    report.set("copies.late", c.intents_late);
    report.set("copies.refused", c.intents_refused);
    report.set("copies.superseded", authority.superseded());
    // The margin distribution: how many ticks of slack each arriving copy had
    // against the seal. `plus1` and up beat it; `zero` and below were late.
    // This is what sizes --intent-lead.
    static constexpr const char* names[lab_margin_buckets] = {
      "minus3_or_worse", "minus2", "minus1", "zero",
      "plus1", "plus2", "plus3", "plus4", "plus5_or_better"};
    const auto& margin = authority.counters().margin;
    uint64_t total = 0, late_side = 0;
    for (size_t i = 0; i < lab_margin_buckets; ++i) {
      report.set(std::string("margin.") + names[i], margin[i]);
      total += margin[i];
      if (i <= 3) late_side += margin[i];
    }
    if (total != 0) report.set("margin.late_fraction", double(late_side) / double(total));
    report.set("run.intent_lead_ticks", schedule.intent_lead_ticks);
    report.summarize_link();
    if (!opts.report.empty() && !report.write(opts.report))
      std::cerr << "NET-LAB-01 authority: cannot write " << opts.report << '\n';
    return code;
  };

  const auto deadline = std::chrono::steady_clock::now() + wall_deadline(opts);
  bool announced_start = false;
  // Waiting for a roster to assemble across machines is the normal state for
  // minutes, and a silent process is indistinguishable from a hung one.
  auto next_report = std::chrono::steady_clock::now();
  while (authority.step()) {
    if (const uint64_t tick = authority.host().state.tick; report.due(tick)) {
      report.mark_sampled(tick);
      const auto measured = authority.measure(0);
      report.sample({tick, monotonic_ms() - wall_origin, measured.ping_ms,
                     measured.quality_local, measured.quality_remote,
                     measured.in_packets_per_second, measured.out_packets_per_second,
                     measured.pending_reliable_bytes});
    }
    if (!authority.started() && !opts.quiet &&
        std::chrono::steady_clock::now() >= next_report) {
      std::cout << "authority waiting: admitted " << authority.admitted() << " of "
                << schedule.followers << " followers\n";
      std::cout.flush();
      next_report = std::chrono::steady_clock::now() + 5s;
    }
    // The marker belongs to the local harness. Writing it in a distributed run
    // would drop a file into whatever directory the operator happened to be in.
    if (!opts.rendezvous.empty() && authority.started() && !announced_start) {
      std::ofstream(started_path(opts.rendezvous)) << "1\n";
      announced_start = true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "NET-LAB-01 authority: wall deadline reached at tick "
                << authority.host().state.tick << '\n';
      return finish("wall_deadline", EXIT_FAILURE);
    }
    std::this_thread::sleep_for(1ms);
  }

  const auto& counters = authority.counters();
  auto& verify = authority.checks();
  verify.require(authority.host().state.tick == authority.final_tick(),
                 "authority did not commit the final tick it announced");
  verify.require(authority.final_tick() >= schedule.final_tick,
                 "the announced final tick went backwards");
  if (schedule.resume_tail_ticks != 0)
    verify.require(authority.final_tick() > schedule.final_tick,
                   "the run was never extended, so a late rejoin was not given a tail to play");
  verify.require(counters.admissions >= schedule.admissions_expected(),
                 "authority did not admit every scheduled join and reconnect");
  if (schedule.followers > 1)
    verify.require(counters.multi_principal_ticks > 0,
                   "no tick ever carried intents from more than one principal, so the "
                   "cross-principal canonical order was never exercised");
  verify.require(counters.intents_accepted > 0, "authority accepted no intent");
  verify.require(counters.intents_duplicate > 0,
                 "the redundant intent window produced no duplicate to drop");
  if (schedule.staleness_tested())
    verify.require(counters.intents_late > 0,
                   "the scheduled stale batch was not refused as late");
  verify.require(counters.recoveries_planned >= 1, "authority planned no recovery");
  verify.require(counters.chunks_sent >= 1, "authority transferred no checkpoint chunk");

  std::cout << "authority checks=" << verify.checks
            << " tick=" << authority.host().state.tick
            << " root=" << lab_root(authority.host())
            << " bundles=" << counters.bundles_sent
            << " intents=" << counters.intents_accepted
            << " duplicate=" << counters.intents_duplicate
            << " late=" << counters.intents_late
            << " refused=" << counters.intents_refused
            << " admissions=" << counters.admissions
            << " from_hold=" << counters.resumes_from_hold
            << " migrated=" << counters.resumes_by_migration
            << " recoveries=" << counters.recoveries_planned
            << " chunks=" << counters.chunks_sent
            << " replay_bundles=" << counters.replay_bundles_sent
            << " ephemeral_bind=" << (bound.ephemeral ? 1 : 0)
            << " multi_principal=" << counters.multi_principal_ticks
            << " no_capacity=" << counters.joins_refused_no_capacity
            << " announced_final=" << authority.final_tick()
            << " superseded=" << authority.superseded() << '\n';
  return finish("completed", EXIT_SUCCESS);
}

int run_intruder(const options& opts) {
  const auto schedule = schedule_with(opts);
  const auto host_address = parse_ipv4(opts.address);
  const auto port = await_port(opts.rendezvous);
  if (!host_address || !port || !await_started(opts.rendezvous)) {
    std::cerr << "NET-LAB-01 intruder: the authority never became ready\n";
    return EXIT_FAILURE;
  }

  verifier verify;
  follower_run intruder(schedule, opts.index, *host_address, *port,
                        ticket_path(opts.rendezvous, opts.index), false);
  const auto deadline = std::chrono::steady_clock::now() + 20s;
  while (!intruder.failed() && !intruder.established() &&
         std::chrono::steady_clock::now() < deadline)
    (void)intruder.step();

  verify.require(!intruder.established(),
                 "a second joiner with an identity already in use was ADMITTED");
  verify.require(intruder.failed(), "the intruder was neither admitted nor refused");
  verify.require(intruder.refusal() == net::session_refusal_reason::no_capacity,
                 "a duplicate identity was refused for the wrong reason");
  std::cout << "intruder checks=" << verify.checks
            << " refusal=" << unsigned(intruder.refusal()) << '\n';
  return lab_intruder_exit;
}

int run_follower(const options& opts) {
  const auto schedule = schedule_with(opts);

  uint32_t host_address = 0;
  uint16_t port = 0;
  std::filesystem::path ticket = opts.ticket;
  if (!opts.endpoint.empty()) {
    const auto parsed = parse_endpoint(opts.endpoint);
    if (!parsed) {
      std::cerr << "NET-LAB-01 follower: '" << opts.endpoint << "' is not A.B.C.D:PORT\n";
      return EXIT_FAILURE;
    }
    host_address = parsed->host;
    port = parsed->port;
    // The ticket is local state, so a distributed follower keeps it beside
    // itself rather than in a directory it shares with nobody.
    if (ticket.empty()) ticket = "netlab01-ticket-" + std::to_string(opts.index);
  } else {
    const auto parsed = parse_ipv4(opts.address);
    if (!parsed) {
      std::cerr << "NET-LAB-01 follower: '" << opts.address << "' is not a dotted quad\n";
      return EXIT_FAILURE;
    }
    host_address = *parsed;
    const auto discovered = await_port(opts.rendezvous);
    if (!discovered) {
      std::cerr << "NET-LAB-01 follower: the authority never published a port\n";
      return EXIT_FAILURE;
    }
    port = *discovered;
    if (ticket.empty()) ticket = ticket_path(opts.rendezvous, opts.index);
  }

  follower_run follower(schedule, opts.index, host_address, port, ticket, opts.resume);

  lab_report report;
  describe_build(report);
  describe_compatibility(report);
  report.set("run.role", std::string_view("follower"));
  report.set("run.scenario", opts.scenario);
  report.set("run.roster", uint64_t(opts.index));
  report.set("run.endpoint", dotted_quad(host_address) + ":" + std::to_string(port));
  report.set("run.resumed", uint64_t(opts.resume ? 1 : 0));
  const uint64_t wall_origin = monotonic_ms();

  const auto finish = [&](const std::string_view outcome, const int code) {
    report.set("run.outcome", outcome);
    report.set("run.wall_ms", monotonic_ms() - wall_origin);
    report.set("state.tick", follower.host().state.tick);
    report.set("state.root", lab_root(follower.host()));
    report.set("state.final_tick_announced", follower.final_tick());
    const auto& c = follower.counters();
    report.set("session.reconnects", c.reconnects);
    report.set("session.recoveries", c.recoveries);
    report.set("session.replayed_ticks", c.replayed_ticks);
    report.set("session.transport_losses", c.transport_losses);
    report.set("session.silence_losses", c.silence_losses);
    report.set("session.warnings", c.warnings);
    report.set("session.refusal", uint64_t(follower.refusal()));
    report.set("session.unrecoverable", uint64_t(follower.unrecoverable() ? 1 : 0));
    report.set("session.unrecoverable_reason", uint64_t(follower.unrecoverable_reason()));
    report.set("bundles.applied", c.bundles_applied);
    report.set("bundles.deferred", c.bundles_deferred);
    report.set("checkpoint.chunks_received", c.chunks_received);
    // The quantity that matters, kept apart from the copy counters it is easy
    // to confuse it with.
    report.set("orders.proposed", c.intents_proposed);
    report.set("orders.landed", c.orders_landed);
    report.set("orders.lost", c.orders_lost);
    report.set("orders.unobserved", c.orders_unobserved);
    report.set("orders.pending_at_exit", follower.orders_pending());
    // The denominator is what was actually decided, not what was proposed:
    // a proposal whose tick never arrived cannot have been lost.
    const uint64_t decided = c.orders_landed + c.orders_lost;
    if (decided != 0)
      report.set("orders.landed_fraction", double(c.orders_landed) / double(decided));
    report.set("run.tick_period_ms_announced", follower.announced_tick_period_ms());
    report.set("run.intent_lead_ticks_announced", follower.announced_lead_ticks());
    report.set("copies.batches_sent", c.batches_sent);
    report.set("copies.superseded", follower.superseded());
    describe_conditions(report, "link.final", follower.conditions());
    report.summarize_link();
    if (!opts.report.empty() && !report.write(opts.report))
      std::cerr << "NET-LAB-01 follower: cannot write " << opts.report << '\n';
    return code;
  };

  const auto deadline = std::chrono::steady_clock::now() + wall_deadline(opts);
  while (follower.step()) {
    if (const uint64_t tick = follower.host().state.tick; report.due(tick)) {
      report.mark_sampled(tick);
      const auto measured = follower.measure_now();
      report.sample({tick, monotonic_ms() - wall_origin, measured.ping_ms,
                     measured.quality_local, measured.quality_remote,
                     measured.in_packets_per_second, measured.out_packets_per_second,
                     measured.pending_reliable_bytes});
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "NET-LAB-01 follower: wall deadline reached at tick "
                << follower.host().state.tick << '\n';
      return finish("wall_deadline", EXIT_FAILURE);
    }
    std::this_thread::sleep_for(1ms);
  }

  const auto& counters = follower.counters();
  auto& verify = follower.checks();
  if (follower.exit_request() != 0) {
    // A scheduled death, printed before leaving so the harness can see how far
    // this incarnation got.
    std::cout << "follower checks=" << verify.checks
              << " roster=" << follower.roster()
              << " tick=" << follower.host().state.tick
              << " root=" << lab_root(follower.host())
              << " exit=scheduled_death\n";
    std::cout.flush();
    return finish("scheduled_death", int(follower.exit_request()));
  }
  if (follower.failed()) {
    std::cerr << "NET-LAB-01 follower: abandoned at tick " << follower.host().state.tick
              << ", refusal=" << unsigned(follower.refusal())
              << ", unrecoverable=" << follower.unrecoverable()
              << ", reason=" << unsigned(follower.unrecoverable_reason())
              << ", transport_loss=" << counters.transport_losses
              << ", silence_loss=" << counters.silence_losses
              << ", reconnects=" << counters.reconnects
              << ", recoveries=" << counters.recoveries
              << ", warnings=" << counters.warnings << '\n';
    return finish("abandoned", EXIT_FAILURE);
  }

  verify.require(follower.host().state.tick == follower.final_tick(),
                 "follower did not reach the final tick the authority announced");
  verify.require(counters.bundles_applied > 0, "follower applied no bundle");
  // Only the followers the schedule actually disturbs must have recovered. The
  // undisturbed one is the control: it has to reach the same tick and the same
  // root WITHOUT a reconnect, which is what proves a neighbour's recovery did
  // not cost it a bundle.
  // A failure counts only where the schedule ENABLED it. Naming the roster
  // position without checking the tick made two untouched followers in the
  // `killed` scenario answerable for a recovery nobody asked them to do.
  const bool disturbed =
    (schedule.closes_someone() && opts.index == schedule.explicit_close_follower) ||
    (schedule.quiets_someone() && opts.index == schedule.quiet_follower) ||
    (schedule.kills_someone() && opts.index == schedule.self_exit_follower);
  if (disturbed) verify.require(counters.recoveries >= 1,
                                "a disturbed follower never recovered transactionally");
  else verify.require(counters.recoveries == 0 && counters.reconnects == 0,
                      "an undisturbed follower was made to reconnect");

  const auto measured = follower.conditions();
  std::cout << "follower checks=" << verify.checks
            << " roster=" << follower.roster()
            << " tick=" << follower.host().state.tick
            << " root=" << lab_root(follower.host())
            << " applied=" << counters.bundles_applied
            << " deferred=" << counters.bundles_deferred
            << " proposed=" << counters.intents_proposed
            << " batches=" << counters.batches_sent
            << " warnings=" << counters.warnings
            << " transport_loss=" << counters.transport_losses
            << " silence_loss=" << counters.silence_losses
            << " reconnects=" << counters.reconnects
            << " recoveries=" << counters.recoveries
            << " replayed=" << counters.replayed_ticks
            << " chunks=" << counters.chunks_received
            << " stale_sent=" << counters.stale_batches_sent
            << " orders_landed=" << counters.orders_landed
            << " orders_lost=" << counters.orders_lost
            << " orders_unobserved=" << counters.orders_unobserved
            << " orders_lost=" << counters.orders_lost
            << " superseded=" << follower.superseded()
            << " announced_final=" << follower.final_tick()
            << " ping_ms=" << measured.ping_ms
            << " quality=" << measured.quality_local
            << " in_pps=" << measured.in_packets_per_second << '\n';
  return finish("completed", EXIT_SUCCESS);
}

// ------------------------------------------------------------------- harness

struct child {
  std::string output;
  int status = -1;
};

std::string quoted(const std::string& value) {
  return "\"" + value + "\"";
}

FILE* spawn(const std::string& command) {
  FILE* stream = LAB_POPEN(command.c_str(), "r");
  if (stream == nullptr) utils::error{}("NET-LAB-01: cannot spawn '{}'", command);
  return stream;
}

child collect(FILE* stream) {
  child result;
  char buffer[512];
  while (std::fgets(buffer, sizeof(buffer), stream) != nullptr) result.output += buffer;
  // The close must happen exactly once and be named before decoding: WIFEXITED
  // and WEXITSTATUS are macros which evaluate their argument more than once.
  const int status = LAB_PCLOSE(stream);
  result.status = LAB_EXIT_CODE(status);
  return result;
}

std::optional<std::string> field(const std::string& text, const std::string_view key) {
  const auto position = text.find(std::string(key) + "=");
  if (position == std::string::npos) return std::nullopt;
  const auto start = position + key.size() + 1;
  const auto end = text.find_first_of(" \n", start);
  return text.substr(start, end == std::string::npos ? end : end - start);
}

size_t verify_scenario(const std::string& self, const std::string& scenario, verifier& verify,
                       const bool quiet) {
  std::error_code code;
  const auto base = std::filesystem::temp_directory_path(code) /
                    ("netlab01-" + scenario + "-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
  std::filesystem::create_directories(base, code);
  verify.require(!code, "the harness could not create its rendezvous directory");

  const auto schedule = schedule_for(scenario);
  size_t intruder_checks = 0;
  const std::string common = quoted(self) + " --rendezvous " + quoted(base.string()) +
                             " --scenario " + scenario;
  // THE RULE, and it has bitten this harness three times: every long-lived
  // child is SPAWNED before any child is COLLECTED. `collect` blocks until its
  // child exits, so a spawn placed after one runs against a session which has
  // already ended. The single exception is the replacement below, whose spawn
  // is triggered by a death and is therefore deliberately late.
  FILE* authority_stream = spawn(common + " --authority");
  std::vector<FILE*> follower_streams;
  for (size_t index = 0; index < schedule.followers; ++index)
    follower_streams.push_back(spawn(common + " --follower --index " + std::to_string(index)));
  // Presents follower 0's identity. It waits for the authority's start marker
  // itself, so spawning it now costs nothing and racing the roster is
  // impossible.
  FILE* intruder_stream = spawn(common + " --intruder --index 0");

  // Output is a few lines per process, far inside one pipe buffer, so reading
  // in sequence cannot deadlock. The intruder is collected first: it is refused
  // within a handshake of the roster being seated, so it exits long before
  // anyone else. Then the dying follower, so its replacement is spawned while
  // its neighbours are still playing.
  const auto intruder = collect(intruder_stream);
  verify.require(intruder.status == lab_intruder_exit,
                 "the duplicate-identity join was not refused as designed");
  if (const auto checks = field(intruder.output, "checks")) intruder_checks = std::stoul(*checks);

  std::vector<child> followers(follower_streams.size());
  FILE* resumed_stream = nullptr;
  const size_t dying = scenario == "killed" ? schedule.self_exit_follower : followers.size();

  if (dying < followers.size()) {
    followers[dying] = collect(follower_streams[dying]);
    verify.require(followers[dying].status == 7,
                   "the follower scheduled to die did not die on schedule");
    resumed_stream = spawn(common + " --follower --resume --index " + std::to_string(dying));
  }
  for (size_t index = 0; index < follower_streams.size(); ++index) {
    if (index == dying) continue;
    followers[index] = collect(follower_streams[index]);
    verify.require(followers[index].status == EXIT_SUCCESS,
                   "a follower nobody disturbed failed");
  }
  if (resumed_stream != nullptr) {
    followers[dying] = collect(resumed_stream);
    verify.require(followers[dying].status == EXIT_SUCCESS,
                   "the relaunched follower did not finish successfully");
  }
  const auto authority = collect(authority_stream);

  if (!quiet) {
    std::cout << intruder.output;
    std::cout << authority.output;
    for (const auto& value : followers) std::cout << value.output;
  }
  verify.require(authority.status == EXIT_SUCCESS, "the authority process failed");
  const auto refused = field(authority.output, "no_capacity");
  verify.require(refused && *refused != "0",
                 "the authority did not record a capacity refusal");

  const auto authority_root = field(authority.output, "root");
  const auto authority_tick = field(authority.output, "tick");
  verify.require(authority_root.has_value(), "the authority did not report its state root");

  size_t child_checks = intruder_checks;
  if (const auto value = field(authority.output, "checks")) child_checks += std::stoul(*value);
  for (size_t index = 0; index < followers.size(); ++index) {
    const auto& value = followers[index];
    verify.require(value.status == EXIT_SUCCESS, "a follower process failed");
    const auto root = field(value.output, "root");
    const auto tick = field(value.output, "tick");
    verify.require(root.has_value(), "a follower did not report its state root");
    verify.require(tick == authority_tick, "a follower stopped at a different tick");
    // The one value the whole stand exists for, now once per follower: N+1
    // independent processes agreeing on the same causal state root.
    verify.require(root == authority_root,
                   "an independent process disagrees about the causal state root");
    if (const auto checks = field(value.output, "checks")) child_checks += std::stoul(*checks);
    (void)index;
  }

  std::filesystem::remove_all(base, code);
  return child_checks;
}

void print_usage(const char* self) {
  std::cout <<
    "NET-LAB-01 multi-process session stand.\n"
    "\n"
    "  " << self << " --verify [--quiet]\n"
    "        Spawn every process locally and assert the whole outcome.\n"
    "\n"
    "  " << self << " --authority --listen HOST:PORT [options]\n"
    "  " << self << " --follower  --connect HOST:PORT --index N [options]\n"
    "        One run across machines. Start the authority first; it prints the\n"
    "        endpoint and then waits for --followers peers before its clock starts.\n"
    "\n"
    "Options:\n"
    "  --followers N     peers the authority waits for (default 3, max 3).\n"
    "                    Launching one follower by hand means --followers 1.\n"
    "  --index N         which roster position this follower claims. Each is a\n"
    "                    distinct identity; two followers must not share one.\n"
    "  --tick-ms N       authority pacing. Travels to the followers in the grant.\n"
    "  --intent-lead N   ticks ahead followers propose (default 4). Set it on the\n"
    "                    AUTHORITY only: it travels in the grant, because the\n"
    "                    authority is what seals the tick. On a real link this is\n"
    "                    the whole margin budget; size it from the margin.* fields\n"
    "                    of a previous run's report.\n"
    "  --final-tick N    run length, in ticks. Give a LAN run tens of seconds.\n"
    "  --deadline-s N    give up after this long (default 60 local, 900 across\n"
    "                    machines).\n"
    "  --suspect-ms N    override the derived silence budgets.\n"
    "  --lost-ms N\n"
    "  --address A.B.C.D interface for the local rendezvous mode.\n"
    "  --rendezvous DIR  local mode: the authority publishes its port in DIR.\n"
    "  --ticket PATH     where a follower keeps its reconnect ticket.\n"
    "  --resume          rejoin using a persisted ticket.\n"
    "  --scenario NAME   continuous (default) or killed.\n"
    "  --quiet\n"
    "\n"
    "Every process ends with one line containing tick= and root=. The run\n"
    "succeeded when every root is equal at the same tick.\n"
    "\n"
    "The session is deliberately unauthenticated (IP_AllowWithoutAuth):\n"
    "standalone GameNetworkingSockets has no certificate authority.\n";
}

int run_verify(const std::string& self, const options& opts) {
  verifier verify;
  size_t child_checks = 0;
  child_checks += verify_scenario(self, "continuous", verify, opts.quiet);
  child_checks += verify_scenario(self, "killed", verify, opts.quiet);
  std::cout << "NET-LAB-01 multi-process session: " << verify.checks << '/' << verify.checks
            << " harness checks, " << child_checks << " in-process checks across "
            << (2 * (lab_max_followers + 2) + 1) << " processes\n";
  return EXIT_SUCCESS;
}

} // namespace

int main(const int argc, const char* const* argv) {
  options opts;
  for (int i = 1; i < argc; ++i) {
    const std::string_view arg = argv[i];
    if (arg == "--verify") opts.which = role::verify;
    else if (arg == "--authority") opts.which = role::authority;
    else if (arg == "--follower") opts.which = role::follower;
    else if (arg == "--intruder") opts.which = role::intruder;
    else if (arg == "--resume") opts.resume = true;
    else if (arg == "--quiet") opts.quiet = true;
    else if (arg == "--scenario" && i + 1 < argc) opts.scenario = argv[++i];
    else if (arg == "--rendezvous" && i + 1 < argc) opts.rendezvous = argv[++i];
    else if (arg == "--address" && i + 1 < argc) opts.address = argv[++i];
    else if (arg == "--index" && i + 1 < argc) opts.index = size_t(std::stoul(argv[++i]));
    else if (arg == "--final-tick" && i + 1 < argc)
      opts.final_tick = uint64_t(std::stoull(argv[++i]));
    else if (arg == "--listen" && i + 1 < argc) opts.endpoint = argv[++i];
    else if (arg == "--connect" && i + 1 < argc) opts.endpoint = argv[++i];
    else if (arg == "--ticket" && i + 1 < argc) opts.ticket = argv[++i];
    else if (arg == "--report" && i + 1 < argc) opts.report = argv[++i];
    else if (arg == "--intent-lead" && i + 1 < argc)
      opts.intent_lead = uint64_t(std::stoull(argv[++i]));
    else if (arg == "--followers" && i + 1 < argc)
      opts.followers = size_t(std::stoul(argv[++i]));
    else if (arg == "--tick-ms" && i + 1 < argc) opts.tick_ms = uint64_t(std::stoull(argv[++i]));
    else if (arg == "--suspect-ms" && i + 1 < argc)
      opts.suspect_ms = uint64_t(std::stoull(argv[++i]));
    else if (arg == "--lost-ms" && i + 1 < argc) opts.lost_ms = uint64_t(std::stoull(argv[++i]));
    else if (arg == "--deadline-s" && i + 1 < argc)
      opts.deadline_s = uint64_t(std::stoull(argv[++i]));
    else if (arg == "--help" || arg == "-h") {
      print_usage(argv[0]);
      return EXIT_SUCCESS;
    }
    else utils::error{}("NET-LAB-01: unknown argument '{}'", arg);
  }

  switch (opts.which) {
    case role::authority:
      if (opts.rendezvous.empty() && opts.endpoint.empty())
        utils::error{}("NET-LAB-01: --authority needs --listen HOST:PORT or --rendezvous DIR");
      return run_authority(opts);
    case role::follower:
      if (opts.rendezvous.empty() && opts.endpoint.empty())
        utils::error{}("NET-LAB-01: --follower needs --connect HOST:PORT or --rendezvous DIR");
      return run_follower(opts);
    case role::intruder:
      if (opts.rendezvous.empty()) utils::error{}("NET-LAB-01: --intruder needs --rendezvous");
      return run_intruder(opts);
    case role::verify: return run_verify(argv[0], opts);
  }
  return EXIT_FAILURE;
}
