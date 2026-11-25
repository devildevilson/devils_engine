#include <chrono>

#include <doctest/doctest.h>

#include "devils_engine/simul/interpolation.h"

namespace {

struct camera_state {
  float x = 0.0f;
  float zoom = 1.0f;
  int mode = 0; // дискретное поле — берётся у новейшего
};

} // namespace

namespace devils_engine::simul {
template <>
struct blend_traits<camera_state> {
  static camera_state mix(const camera_state& a, const camera_state& b, const float t) noexcept {
    camera_state o = b;
    o.x = a.x + (b.x - a.x) * t;
    o.zoom = a.zoom + (b.zoom - a.zoom) * t;
    return o;
  }
};
} // namespace devils_engine::simul

using devils_engine::simul::render_frame_clock;
using devils_engine::simul::value_interpolator;

TEST_CASE("value_interpolator blends the last two snapshots of one value [simul::interpolation]") {
  value_interpolator<camera_state> camera;
  CHECK_FALSE(camera.has_data());
  CHECK(camera.resolve().zoom == 1.0f);

  camera.push(camera_state{0.0f, 1.0f, 0}, 100);
  CHECK(camera.resolve().x == 0.0f); // нет prev — cur как есть

  camera.push(camera_state{10.0f, 3.0f, 7}, 100);
  CHECK(camera.resolve().x == 0.0f); // на приходе снапшота alpha = 0 — показываем prev
  CHECK(camera.resolve().mode == 7);
  camera.advance(50);
  CHECK(camera.resolve().x == doctest::Approx(5.0f));
  CHECK(camera.resolve().zoom == doctest::Approx(2.0f));
  camera.advance(500);
  CHECK(camera.resolve().x == doctest::Approx(10.0f)); // экстраполяции нет

  camera.forget_previous();
  camera.advance(0);
  CHECK(camera.resolve().x == 10.0f);
}

TEST_CASE("render_frame_clock stops on pause and does not bring the pause back [simul::interpolation]") {
  using namespace std::chrono_literals;
  // Моменты кадров заданы явно: настенное время зависит от планировщика (на Windows sleep_for
  // спит квантами ~16 мс), и порог «пауза не попала» был бы ложным.
  std::chrono::steady_clock::time_point t{};
  render_frame_clock clock;
  clock.begin_frame(true, t);
  CHECK(clock.elapsed() == 0); // первый кадр
  t += 2ms;
  clock.begin_frame(true, t);
  CHECK(clock.elapsed() == 2000);
  CHECK(clock.for_stream(true) == 0); // кадр снапшота потока
  CHECK(clock.for_stream(false) == clock.elapsed());

  clock.begin_frame(false, t);
  CHECK(clock.elapsed() == 0);
  t += 5s;
  clock.begin_frame(false, t);
  CHECK(clock.elapsed() == 0);
  t += 1ms;
  clock.begin_frame(true, t); // первый кадр после паузы
  CHECK(clock.elapsed() == 0);
  t += 1ms;
  clock.begin_frame(true, t);
  CHECK(clock.elapsed() == 1000); // длина паузы не попала

  clock.reset();
  t += 3ms;
  clock.begin_frame(true, t);
  CHECK(clock.elapsed() == 0);

  // Настенный вариант: тот же счёт от steady_clock.
  render_frame_clock wall;
  wall.begin_frame(true);
  wall.begin_frame(true);
  CHECK(wall.elapsed() < size_t(60) * 1000 * 1000);
}
