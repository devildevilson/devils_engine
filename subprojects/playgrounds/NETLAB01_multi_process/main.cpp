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

enum class role : uint8_t { verify, authority, follower };

struct options {
  role which = role::verify;
  std::string scenario = "continuous";
  std::filesystem::path rendezvous;
  bool resume = false;
  bool quiet = false;
};

lab_schedule schedule_for(const std::string_view scenario) {
  if (scenario == "continuous") return lab_continuous_schedule();
  if (scenario == "killed") return lab_killed_schedule();
  utils::error{}("NET-LAB-01: unknown scenario '{}'", scenario);
  return {};
}

std::filesystem::path port_path(const std::filesystem::path& base) {
  return base / "port";
}
std::filesystem::path ticket_path(const std::filesystem::path& base) {
  return base / "ticket";
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
  const auto schedule = schedule_for(opts.scenario);
  authority_run authority(schedule);
  const auto bound = authority.listen();
  if (!publish_port(opts.rendezvous, bound.port)) {
    std::cerr << "NET-LAB-01 authority: cannot publish its port\n";
    return EXIT_FAILURE;
  }

  const auto deadline = std::chrono::steady_clock::now() + 60s;
  while (authority.step()) {
    if (std::chrono::steady_clock::now() >= deadline) {
      std::cerr << "NET-LAB-01 authority: wall deadline reached at tick "
                << authority.host().state.tick << '\n';
      return EXIT_FAILURE;
    }
    std::this_thread::sleep_for(1ms);
  }

  const auto& counters = authority.counters();
  auto& verify = authority.checks();
  verify.require(authority.host().state.tick == schedule.final_tick,
                 "authority did not commit its scheduled final tick");
  verify.require(counters.admissions >= 2, "authority admitted no reconnect at all");
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
            << " ephemeral_bind=" << (bound.ephemeral ? 1 : 0) << '\n';
  return EXIT_SUCCESS;
}

int run_follower(const options& opts) {
  const auto schedule = schedule_for(opts.scenario);
  const auto port = await_port(opts.rendezvous);
  if (!port) {
    std::cerr << "NET-LAB-01 follower: the authority never published a port\n";
    return EXIT_FAILURE;
  }

  follower_run follower(schedule, *port, ticket_path(opts.rendezvous), opts.resume);
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

  verify.require(follower.host().state.tick == schedule.final_tick,
                 "follower did not reach the scheduled final tick");
  verify.require(counters.recoveries >= 1, "follower never recovered transactionally");
  verify.require(counters.bundles_applied > 0, "follower applied no bundle");

  std::cout << "follower checks=" << verify.checks
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
            << " stale_sent=" << counters.stale_batches_sent << '\n';
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

  const std::string common = quoted(self) + " --rendezvous " + quoted(base.string()) +
                             " --scenario " + scenario;
  FILE* authority_stream = spawn(common + " --authority");
  FILE* follower_stream = spawn(common + " --follower");
  // Output is a few lines, far inside one pipe buffer, so reading in sequence
  // cannot deadlock. The ORDER matters though: the follower is collected first,
  // because in the `killed` scenario its replacement has to be spawned while
  // the authority is still running.
  auto follower = collect(follower_stream);
  if (scenario == "killed") {
    verify.require(follower.status == 7,
                   "the first follower incarnation did not die on schedule");
    FILE* resumed_stream = spawn(common + " --follower --resume");
    follower = collect(resumed_stream);
  }
  const auto authority = collect(authority_stream);

  if (!quiet) {
    std::cout << authority.output;
    std::cout << follower.output;
  }
  if (scenario == "killed")
    verify.require(follower.status == EXIT_SUCCESS,
                   "the relaunched follower did not finish successfully");
  verify.require(authority.status == EXIT_SUCCESS, "the authority process failed");
  verify.require(follower.status == EXIT_SUCCESS, "the follower process failed");

  const auto authority_root = field(authority.output, "root");
  const auto follower_root = field(follower.output, "root");
  const auto authority_tick = field(authority.output, "tick");
  const auto follower_tick = field(follower.output, "tick");
  verify.require(authority_root && follower_root, "a process did not report its state root");
  verify.require(authority_tick == follower_tick,
                 "the two processes stopped at different ticks");
  verify.require(authority_root == follower_root,
                 "two independent processes disagree about the causal state root");

  size_t child_checks = 0;
  if (const auto value = field(authority.output, "checks")) child_checks += std::stoul(*value);
  if (const auto value = field(follower.output, "checks")) child_checks += std::stoul(*value);

  std::filesystem::remove_all(base, code);
  return child_checks;
}

int run_verify(const std::string& self, const options& opts) {
  verifier verify;
  size_t child_checks = 0;
  child_checks += verify_scenario(self, "continuous", verify, opts.quiet);
  child_checks += verify_scenario(self, "killed", verify, opts.quiet);
  std::cout << "NET-LAB-01 multi-process session: " << verify.checks << '/' << verify.checks
            << " harness checks, " << child_checks << " in-process checks across 5 processes\n";
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
    else if (arg == "--resume") opts.resume = true;
    else if (arg == "--quiet") opts.quiet = true;
    else if (arg == "--scenario" && i + 1 < argc) opts.scenario = argv[++i];
    else if (arg == "--rendezvous" && i + 1 < argc) opts.rendezvous = argv[++i];
    else utils::error{}("NET-LAB-01: unknown argument '{}'", arg);
  }

  switch (opts.which) {
    case role::authority:
      if (opts.rendezvous.empty()) utils::error{}("NET-LAB-01: --authority needs --rendezvous");
      return run_authority(opts);
    case role::follower:
      if (opts.rendezvous.empty()) utils::error{}("NET-LAB-01: --follower needs --rendezvous");
      return run_follower(opts);
    case role::verify: return run_verify(argv[0], opts);
  }
  return EXIT_FAILURE;
}
