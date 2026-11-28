#include <exception>
#include <iostream>
#include <string>

#include "devils_engine/playground/cli.h"

#include "replay.h"

using namespace devils_engine;

int main(const int argc, const char* const* argv) {
  CLI::App app{"PHY03 - CharacterVirtual, ragdoll, platform braking and FPS camera"};
  bool verify = false;
  bool dump = false;
  uint32_t fps = 60;
  std::string replay = "strong";
  app.add_flag("--verify", verify, "Run the headless contract checks");
  app.add_flag("--dump", dump, "Print replay progress");
  app.add_option("--fps", fps, "Replay frame rate")->check(CLI::Range(15, 240))->capture_default_str();
  app.add_option("--replay", replay, "Input recording")
    ->check(CLI::IsMember({"strong", "weak", "low", "push", "rescue", "overlap", "stack"}));
  if (const auto exit = playground::parse_cli(app, argc, argv)) return *exit;

  try {
    phy03::jolt_runtime runtime;
    if (verify) {
      phy03::verify();
    } else {
      const auto scenario = phy03::replay_from_name(replay);
      const auto hash = phy03::run_replay(scenario, fps, dump);
      std::cout << "PHY03 " << replay << " fingerprint " << hash << '\n';
    }
    return 0;
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
