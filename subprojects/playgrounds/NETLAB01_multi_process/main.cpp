#include "authority.h"
#include "follower.h"

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
  std::filesystem::path rendezvous;
  // Loopback by default. A LAN run is this flag plus a reachable interface, so
  // moving to another machine is a command line rather than a code change.
  std::string address = "127.0.0.1";
  size_t index = 0;
  // Overrides the scenario's run length. A LAN session wants a longer run than
  // a registered test can afford, and the backend's connection-quality window
  // is longer than the test's whole run.
  uint64_t final_tick = 0;
  bool resume = false;
  bool quiet = false;
};

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

lab_schedule schedule_for(const std::string_view scenario) {
  if (scenario == "continuous") return lab_continuous_schedule();
  if (scenario == "killed") return lab_killed_schedule();
  utils::error{}("NET-LAB-01: unknown scenario '{}'", scenario);
  return {};
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
  auto schedule = schedule_for(opts.scenario);
  if (opts.final_tick != 0) schedule.final_tick = opts.final_tick;
  const auto host_address = parse_ipv4(opts.address);
  if (!host_address) {
    std::cerr << "NET-LAB-01 authority: '" << opts.address << "' is not a dotted quad\n";
    return EXIT_FAILURE;
  }
  authority_run authority(schedule);
  const auto bound = authority.listen(*host_address);
  if (!publish_port(opts.rendezvous, bound.port)) {
    std::cerr << "NET-LAB-01 authority: cannot publish its port\n";
    return EXIT_FAILURE;
  }

  const auto deadline = std::chrono::steady_clock::now() + 60s;
  bool announced_start = false;
  while (authority.step()) {
    if (authority.started() && !announced_start) {
      std::ofstream(started_path(opts.rendezvous)) << "1\n";
      announced_start = true;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "NET-LAB-01 authority: wall deadline reached at tick "
                << authority.host().state.tick << '\n';
      return EXIT_FAILURE;
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
  verify.require(counters.admissions >= schedule.expected_admissions,
                 "authority did not admit every scheduled join and reconnect");
  verify.require(counters.multi_principal_ticks > 0,
                 "no tick ever carried intents from more than one principal, so the "
                 "cross-principal canonical order was never exercised");
  verify.require(counters.intents_accepted > 0, "authority accepted no intent");
  verify.require(counters.intents_duplicate > 0,
                 "the redundant intent window produced no duplicate to drop");
  if (schedule.stale_batch_tick != 0)
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
            << " announced_final=" << authority.final_tick() << '\n';
  return EXIT_SUCCESS;
}

int run_intruder(const options& opts) {
  const auto schedule = schedule_for(opts.scenario);
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
  auto schedule = schedule_for(opts.scenario);
  if (opts.final_tick != 0) schedule.final_tick = opts.final_tick;
  const auto host_address = parse_ipv4(opts.address);
  if (!host_address) {
    std::cerr << "NET-LAB-01 follower: '" << opts.address << "' is not a dotted quad\n";
    return EXIT_FAILURE;
  }
  const auto port = await_port(opts.rendezvous);
  if (!port) {
    std::cerr << "NET-LAB-01 follower: the authority never published a port\n";
    return EXIT_FAILURE;
  }

  follower_run follower(schedule, opts.index, *host_address, *port,
                        ticket_path(opts.rendezvous, opts.index), opts.resume);
  const auto deadline = std::chrono::steady_clock::now() + 60s;
  while (follower.step()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "NET-LAB-01 follower: wall deadline reached at tick "
                << follower.host().state.tick << '\n';
      return EXIT_FAILURE;
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
    return int(follower.exit_request());
  }
  if (follower.failed()) {
    std::cerr << "NET-LAB-01 follower: abandoned at tick " << follower.host().state.tick
              << ", refusal=" << unsigned(follower.refusal())
              << ", unrecoverable=" << follower.unrecoverable()
              << ", transport_loss=" << counters.transport_losses
              << ", silence_loss=" << counters.silence_losses
              << ", reconnects=" << counters.reconnects
              << ", recoveries=" << counters.recoveries
              << ", warnings=" << counters.warnings << '\n';
    return EXIT_FAILURE;
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
    (schedule.explicit_close_tick != 0 && opts.index == schedule.explicit_close_follower) ||
    (schedule.quiet_from_tick != 0 && opts.index == schedule.quiet_follower) ||
    (schedule.self_exit_tick != 0 && opts.index == schedule.self_exit_follower);
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
            << " announced_final=" << follower.final_tick()
            << " ping_ms=" << measured.ping_ms
            << " quality=" << measured.quality_local
            << " in_pps=" << measured.in_packets_per_second << '\n';
  return EXIT_SUCCESS;
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
    else utils::error{}("NET-LAB-01: unknown argument '{}'", arg);
  }

  switch (opts.which) {
    case role::authority:
      if (opts.rendezvous.empty()) utils::error{}("NET-LAB-01: --authority needs --rendezvous");
      return run_authority(opts);
    case role::follower:
      if (opts.rendezvous.empty()) utils::error{}("NET-LAB-01: --follower needs --rendezvous");
      return run_follower(opts);
    case role::intruder:
      if (opts.rendezvous.empty()) utils::error{}("NET-LAB-01: --intruder needs --rendezvous");
      return run_intruder(opts);
    case role::verify: return run_verify(argv[0], opts);
  }
  return EXIT_FAILURE;
}
