#include <exception>
#include <iostream>

#include "devils_engine/playground/cli.h"
#include "devils_engine/utils/error.h"

#include "frames.h"
#include "server_zones.h"

using namespace devils_engine;

int main(const int argc, const char* const* argv) {
  CLI::App app{"PHY04 - chunk rebasing and ragdoll transfer through a moving cabin"};
  bool verify = false;
  bool dump = false;
  bool turning = false;
  bool zones = false;
  app.add_flag("--verify", verify, "Verify the selected experiment");
  app.add_flag("--dump", dump, "Print coordinates at events and every half-second");
  auto* turn_option = app.add_flag("--turn", turning, "Rotate the cabin during exit");
  app.add_flag("--zones", zones, "Run the two authoritative server zones and their merge")->excludes(turn_option);
  if (const auto result = playground::parse_cli(app, argc, argv)) return *result;

  try {
    const phy03::jolt_runtime runtime;
    if (verify) {
      if (zones) phy04::verify_zones();
      else phy04::verify();
      return 0;
    }
    if (zones) {
      phy04::server_experiment sim;
      while (sim.current().tick < phy04::zone_step_budget) {
        sim.step();
        const auto& now = sim.current();
        if (dump && (now.tick % 30 == 0 || now.tick == now.merged_tick ||
                     now.tick == now.cargo_contact_tick || now.tick == now.player_contact_tick))
          phy04::print_zones(now);
      }
      if (sim.current().player_contact_tick == 0 || sim.current().cargo_contact_tick == 0)
        utils::error{}("PHY04 server tick {}: replay ended before both inter-group contacts", sim.current().tick);
      phy04::print_zones(sim.current());
      return 0;
    }
    phy04::experiment sim(turning);
    while (sim.current().tick < phy04::step_budget && sim.current().stage != phy04::phase::finished) {
      sim.step();
      const auto& now = sim.current();
      const bool event = now.rebase_tick == now.tick || now.stage != sim.previous().stage;
      if (dump && (now.tick % 30 == 0 || event))
        phy04::print_sample(now);
      if (dump && event) phy04::print_points(now);
    }
    const auto& result = sim.current();
    if (result.stage != phy04::phase::finished)
      utils::error{}("PHY04 tick {}: replay ended during phase '{}'", result.tick, phy04::phase_name(result.stage));
    std::cout << "PHY04 rebase " << result.rebase_tick << ", entry " << result.enter_tick
              << ", deck push " << result.kick_tick << ", exit " << result.leave_tick << ", finish " << result.tick << '\n';
  } catch (const std::exception& error) {
    std::cerr << error.what() << '\n';
    return 1;
  }
}
