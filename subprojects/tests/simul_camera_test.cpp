#include <doctest/doctest.h>

#include "devils_engine/simul/camera.h"
#include "devils_engine/utils/core.h"

namespace simul = devils_engine::simul;
namespace utils = devils_engine::utils;

TEST_CASE("camera_config refuses unknown words instead of defaulting [simul::camera]") {
  simul::camera_config config;
  CHECK_NOTHROW(simul::validate_camera_config(config, "test"));

  auto bad = config;
  bad.projection = "perspective";
  CHECK_THROWS_AS(simul::validate_camera_config(bad, "test"), utils::critical_error);
  bad = config;
  bad.controller = "orbit";
  CHECK_THROWS_AS(simul::validate_camera_config(bad, "test"), utils::critical_error);
  bad = config;
  bad.limits = "box";
  CHECK_THROWS_AS(simul::validate_camera_config(bad, "test"), utils::critical_error);
  bad = config;
  bad.body = "capsule"; // тело появится вместе с физикой главного потока
  CHECK_THROWS_AS(simul::validate_camera_config(bad, "test"), utils::critical_error);
  bad = config;
  bad.half_width = 0.0f;
  CHECK_THROWS_AS(simul::validate_camera_config(bad, "test"), utils::critical_error);
}

TEST_CASE("step_camera pans in screen half-widths per second and honours limits [simul::camera]") {
  simul::camera_config config;
  config.half_width = 10.0f;
  config.pan_speed = 1.5f;
  simul::camera2d camera;
  camera.half_width = config.half_width;
  camera.center = {50.0f, 50.0f};

  simul::step_camera(camera, config, {1.0f, 0.0f}, 0.5f, {0.0f, 0.0f}, {100.0f, 100.0f});
  CHECK(camera.center.x == doctest::Approx(57.5f)); // 10 * 1.5 * 0.5
  // Диагональ нормируется: скорость та же.
  simul::step_camera(camera, config, {1.0f, 1.0f}, 1.0f, {0.0f, 0.0f}, {100.0f, 100.0f});
  CHECK(camera.center.x == doctest::Approx(57.5f + 15.0f * 0.70710678f));

  simul::step_camera(camera, config, {1.0f, 0.0f}, 100.0f, {0.0f, 0.0f}, {100.0f, 100.0f});
  CHECK(camera.center.x == 100.0f); // limits = world

  config.limits = "none";
  simul::step_camera(camera, config, {1.0f, 0.0f}, 1.0f, {0.0f, 0.0f}, {100.0f, 100.0f});
  CHECK(camera.center.x == doctest::Approx(115.0f));

  config.controller = "fixed";
  simul::step_camera(camera, config, {-1.0f, 0.0f}, 1.0f, {0.0f, 0.0f}, {100.0f, 100.0f});
  CHECK(camera.center.x == doctest::Approx(115.0f));
}

TEST_CASE("published_cameras bounds cover what the render may still draw [simul::camera]") {
  const auto at = [](const float x, const float y) {
    simul::camera2d c;
    c.center = {x, y};
    c.half_width = 10.0f;
    c.aspect = 2.0f;
    return c;
  };
  simul::published_cameras published;
  const auto current = at(100.0f, 50.0f);
  // Без истории — текущая камера.
  auto b = published.bounds(current);
  CHECK(b.min.x == doctest::Approx(90.0f));
  CHECK(b.max.x == doctest::Approx(110.0f));

  // Камера идёт вправо: отстающий край — у старых снапшотов.
  published.push(at(96.0f, 50.0f));
  published.push(at(98.0f, 50.0f));
  b = published.bounds(current);
  CHECK(b.min.x == doctest::Approx(86.0f));
  CHECK(b.max.x == doctest::Approx(110.0f));
  CHECK(b.min.y == doctest::Approx(45.0f));

  // Глубина ограничена: самый старый снапшот выпадает.
  published.push(at(99.0f, 50.0f));
  published.push(at(99.5f, 50.0f));
  b = published.bounds(current);
  CHECK(b.min.x == doctest::Approx(88.0f));

  // Прыжок (сброс камеры) в объединение не входит.
  published.push(at(0.0f, 0.0f));
  b = published.bounds(current);
  CHECK(b.min.x == doctest::Approx(89.0f));
  CHECK(b.min.y == doctest::Approx(45.0f));

  published.clear();
  b = published.bounds(current);
  CHECK(b.min.x == doctest::Approx(90.0f));
}
