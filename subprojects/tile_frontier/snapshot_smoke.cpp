// Smoke: снапшот РЕАЛЬНЫХ компонентов tile_frontier (glm-адаптеры + межсущностные ссылки).
// Строит сценарий хищник/жертва/еда/препятствие, pack -> unpack в чистый мир, сверяет.
// Плоский main (без doctest): печатает результат, код возврата 0/1.
#include <cstdio>
#include <cmath>

#include "core/actor_snapshot.h"

using namespace devils_engine;
namespace tf = tile_frontier::core;

static int failures = 0;
#define CHECK(cond) do { if (!(cond)) { std::printf("  FAIL: %s (line %d)\n", #cond, __LINE__); ++failures; } } while (0)

int main() {
  aesthetics::world w;

  const auto predator = w.gen_entityid();
  const auto prey     = w.gen_entityid();
  const auto food     = w.gen_entityid();
  const auto rock     = w.gen_entityid();

  // --- хищник ---
  w.create<tf::actor_position>(predator, tf::actor_position{ glm::vec2{1.5f, 2.5f} });
  w.create<tf::actor_velocity>(predator, tf::actor_velocity{ glm::vec2{-0.5f, 0.25f} });
  w.create<tf::actor_brain>(predator, tf::actor_brain{ 12345u, 7u, 2.0f });
  w.create<tf::actor_visual>(predator, tf::actor_visual{ 3u, { 0xff00ff00u }, 1.25f });
  w.create<tf::actor_cognition>(predator, tf::actor_cognition{ 999u });
  w.create<tf::actor_drives>(predator, tf::actor_drives{ 0.8f, 0.1f });
  w.create<tf::actor_state>(predator, tf::actor_state{ 0xDEADBEEFULL });
  {
    auto* p = w.create<tf::actor_perception>(predator);
    p->prey_pos = glm::vec2{ 10.0f, 20.0f };
    p->prey_id = prey;   // ССЫЛКА на другую сущность
    p->has_prey = true;
  }
  w.create<tf::actor_eating>(predator, tf::actor_eating{ prey, 500u }); // ест жертву

  // --- жертва ---
  w.create<tf::actor_position>(prey, tf::actor_position{ glm::vec2{10.0f, 20.0f} });
  w.create<tf::actor_drives>(prey, tf::actor_drives{ 0.2f, 0.9f });
  w.create<tf::actor_grabbed>(prey, tf::actor_grabbed{ predator }); // схвачена хищником

  // --- еда + препятствие ---
  w.create<tf::actor_position>(food, tf::actor_position{ glm::vec2{5.0f, 5.0f} });
  w.create<tf::food_item>(food, tf::food_item{ 3.5f });
  w.create<tf::actor_position>(rock, tf::actor_position{ glm::vec2{-4.0f, -4.0f} });
  w.create<tf::obstacle>(rock, tf::obstacle{ 2.0f });

  // снапшот -> байты -> чистый мир
  const auto bytes = aesthetics::serial::pack(&w, aesthetics::serial::disk_policy);
  std::printf("packed %zu bytes\n", bytes.size());

  aesthetics::world w2;
  if (!aesthetics::serial::unpack(bytes, &w2)) { std::printf("unpack FAILED\n"); return 1; }

  // glm-поля через адаптер + скаляры
  CHECK(w2.get<tf::actor_position>(predator)->value.x == 1.5f);
  CHECK(w2.get<tf::actor_position>(predator)->value.y == 2.5f);
  CHECK(w2.get<tf::actor_velocity>(predator)->value.x == -0.5f);
  CHECK(w2.get<tf::actor_brain>(predator)->seed == 12345u);
  CHECK(w2.get<tf::actor_brain>(predator)->speed == 2.0f);
  CHECK(w2.get<tf::actor_visual>(predator)->color.value == 0xff00ff00u);
  CHECK(std::abs(w2.get<tf::actor_drives>(predator)->hunger - 0.8f) < 1e-6f);
  CHECK(w2.get<tf::actor_state>(predator)->state == 0xDEADBEEFULL);

  // межсущностные ссылки (полный id с версией)
  CHECK(w2.get<tf::actor_perception>(predator)->prey_id == prey);
  CHECK(w2.get<tf::actor_perception>(predator)->has_prey == true);
  CHECK(w2.get<tf::actor_perception>(predator)->prey_pos.y == 20.0f);
  CHECK(w2.get<tf::actor_eating>(predator)->target == prey);
  CHECK(w2.get<tf::actor_eating>(predator)->until_tick == 500u);
  CHECK(w2.get<tf::actor_grabbed>(prey)->by == predator);
  CHECK(w2.get<tf::actor_position>(w2.get<tf::actor_eating>(predator)->target) != nullptr); // ссылка разрешается в живую сущность

  // еда / препятствие
  CHECK(std::abs(w2.get<tf::food_item>(food)->nutrition - 3.5f) < 1e-6f);
  CHECK(std::abs(w2.get<tf::obstacle>(rock)->radius - 2.0f) < 1e-6f);

  // генератор продолжает с того же места
  CHECK(w2.gen_entityid() == w.gen_entityid());

  if (failures) { std::printf("SMOKE FAILED: %d check(s)\n", failures); return 1; }
  std::printf("SMOKE OK: real tile_frontier components round-trip (glm adapters + entity refs + fingerprint guard)\n");
  return 0;
}
