#include <array>
#include <cstdio>
#include <cstdlib>

#include "cardgame/combat.h"

namespace cg = cardgame::core;

namespace {

[[noreturn]] void fail(const char* message) {
  std::fprintf(stderr, "cardgame_headless_smoke: %s\n", message);
  std::exit(EXIT_FAILURE);
}

void check(const bool value, const char* message) {
  if (!value) {
    fail(message);
  }
}

// Fake render thread. It acknowledges presentation commands only between main-thread updates:
// start -> gameplay marker, result -> finished marker.
void drive_to_player(cg::combat& game, uint64_t& engine_tick) {
  for (uint32_t guard = 0; guard < 256; ++guard) {
    game.update(++engine_tick);
    const auto commands = game.take_presentation_commands();
    for (const auto& command : commands) {
      const auto event = command.kind == cg::presentation_command_kind::start
                           ? devils_engine::simul::presentation_event_kind::gameplay
                           : devils_engine::simul::presentation_event_kind::finished;
      check(game.notify_presentation(command.task, event),
            "fake render produced an unexpected presentation event");
    }
    if (game.awaiting_player()) {
      return;
    }
    check(!game.faulted(), "presentation pipeline faulted");
  }
  fail("pipeline did not reach player input within the guard budget");
}

void submit_and_drive(cg::combat& game, const cg::player_intent intent, uint64_t& tick) {
  check(game.submit(intent), "player intent was rejected at an input boundary");
  drive_to_player(game, tick);
}

} // namespace

int main() {
  cg::combat headless(cg::run_mode::headless);
  cg::combat animated(cg::run_mode::animated);
  uint64_t headless_tick = 0;
  uint64_t animated_tick = 0;
  drive_to_player(headless, headless_tick);
  drive_to_player(animated, animated_tick);

  // Three turns. quick_strike increments the player-action coordinate but not countdown;
  // end_turn creates forced countdown pulses that are not player actions.
  const std::array script{
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::strike, 1},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::quick_strike, 2},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::strike, 3},
    cg::player_intent{cg::player_intent_kind::end_turn, cg::card_kind::strike, 4},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::strike, 5},
    cg::player_intent{cg::player_intent_kind::end_turn, cg::card_kind::strike, 6},
    cg::player_intent{cg::player_intent_kind::play_card, cg::card_kind::quick_strike, 7},
    cg::player_intent{cg::player_intent_kind::end_turn, cg::card_kind::strike, 8},
  };

  for (const auto intent : script) {
    submit_and_drive(headless, intent, headless_tick);
    submit_and_drive(animated, intent, animated_tick);
    check(headless.state() == animated.state(),
          "animated and headless authoritative states diverged");
  }

  check(headless.state().turn_index == 4, "script did not cross three turn boundaries");
  check(headless.state().player_action_index == 5,
        "player action coordinate counted a forced pulse or end-turn");
  check(headless.state().countdown_pulse_index == 6,
        "countdown pulse coordinate did not distinguish quick actions and forced pulses");

  // Resume while the first player attack is flying and has not reached its gameplay point.
  cg::combat in_flight(cg::run_mode::animated);
  uint64_t in_flight_tick = 0;
  drive_to_player(in_flight, in_flight_tick);
  check(in_flight.submit(
          {cg::player_intent_kind::play_card, cg::card_kind::strike, 1}),
        "could not submit in-flight snapshot action");
  in_flight.update(++in_flight_tick); // publishes start and waits; no gameplay marker delivered
  check(in_flight.state().enemy_hp == 100, "damage committed before gameplay marker");
  check(in_flight.waiting_presentation(), "animated attack is not waiting at gameplay marker");

  const auto snap = in_flight.save();
  cg::combat resumed(cg::run_mode::headless);
  resumed.load(snap);
  uint64_t resumed_tick = 0;
  drive_to_player(resumed, resumed_tick);

  cg::combat control(cg::run_mode::headless);
  uint64_t control_tick = 0;
  drive_to_player(control, control_tick);
  submit_and_drive(control,
                   {cg::player_intent_kind::play_card, cg::card_kind::strike, 1},
                   control_tick);
  check(resumed.state() == control.state(),
        "resume at gameplay marker did not commit the action exactly once");

  std::printf(
    "cardgame headless/animated identity: turns=%llu actions=%llu pulses=%llu player_hp=%d enemy_hp=%d\n",
    static_cast<unsigned long long>(headless.state().turn_index),
    static_cast<unsigned long long>(headless.state().player_action_index),
    static_cast<unsigned long long>(headless.state().countdown_pulse_index),
    headless.state().player_hp,
    headless.state().enemy_hp);
  return EXIT_SUCCESS;
}
