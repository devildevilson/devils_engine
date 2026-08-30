#include <algorithm>
#include <cstdint>
#include <format>
#include <iostream>
#include <string>
#include <string_view>

#include <glm/geometric.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/mat3x3.hpp>

#include "planet.h"
#include "viewer.h"

namespace {

using namespace devils_engine;

void usage() {
  std::cout << "PF10 functional planet\n"
               "  --view                 открыть первое отображение планеты (по умолчанию)\n"
               "  --verify               проверить surface/province contracts без Vulkan\n"
               "  --frames=N             закрыть viewer после N кадров\n"
               "  --shot=PATH            сохранить последний кадр в PPM\n"
               "  --mesh=N               клеток на грань cube-sphere (32..512, default 256)\n"
               "  --fixed-rotation       детерминированный угол глобуса для кадров\n"
               "  --validation           включить Vulkan validation layers\n";
}

bool prefixed(const std::string_view argument, const std::string_view prefix, std::string& value) {
  if (!argument.starts_with(prefix)) return false;
  value = argument.substr(prefix.size());
  return true;
}

int verify() {
  constexpr uint32_t samples = 600000;
  const auto first = pf10::survey_planet(samples);
  const auto second = pf10::survey_planet(samples);
  uint32_t passed = 0;
  uint32_t failed = 0;
  auto check = [&](const bool condition, const std::string_view name) {
    std::cout << (condition ? "PASS " : "FAIL ") << name << '\n';
    condition ? ++passed : ++failed;
  };

  check(first.land_regions >= 3000 && first.land_regions <= 5000,
        std::format("playable land provinces are in [3000,5000] (sampled {})", first.land_regions));
  check(first.water_regions >= 3 && first.water_regions <= 8,
        std::format("water regions stay large and few ({})", first.water_regions));
  check(first.polar_regions == 2, "north and south are two explicit non-playable regions");
  check(first.sampled_min_height >= pf10::minimum_height - 1.0e-6f &&
          first.sampled_max_height <= pf10::maximum_height + 1.0e-6f,
        std::format("height is bounded [{:.5f},{:.5f}]", first.sampled_min_height, first.sampled_max_height));
  check(first.sampled_max_height - first.sampled_min_height > 0.075f,
        "geometry has a visible radial height range");
  check(first.fingerprint == second.fingerprint, std::format("deterministic fingerprint 0x{:016x}", first.fingerprint));

  const glm::vec3 probe = glm::normalize(glm::vec3(0.37f, 0.51f, -0.78f));
  const float original_height = pf10::surface_height(probe);
  const auto original_region = pf10::sample_region(probe);
  const glm::mat3 rotation = glm::mat3(glm::rotate(glm::mat4(1.0f), glm::radians(137.0f), glm::normalize(glm::vec3(0.2f, 1.0f, 0.3f))));
  const glm::vec3 world = rotation * pf10::surface_position(probe);
  const glm::vec3 recovered = glm::transpose(rotation) * glm::normalize(world);
  check(std::abs(pf10::surface_height(recovered) - original_height) < 1.0e-6f,
        "height is planet-local and survives globe rotation");
  check(pf10::sample_region(recovered).id == original_region.id,
        "province identity is planet-local and survives globe rotation");

  const auto landmarks = pf10::make_landmarks(24);
  check(landmarks.size() == 24, "fixture has city/wonder/construction anchors");
  check(std::ranges::all_of(landmarks, [](const pf10::landmark& item) {
          return pf10::sample_region(glm::vec3(item.direction_height)).id == item.region_id;
        }), "every object anchor belongs to its recorded province");

  const auto centre_hit = pf10::intersect_surface(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 0.0f, -1.0f));
  check(centre_hit.hit && centre_hit.region.id != pf10::no_region,
        "visible displaced surface resolves to a selectable navigation region");
  check(!pf10::intersect_surface(glm::vec3(0.0f, 0.0f, 3.0f), glm::vec3(0.0f, 1.0f, 0.0f)).hit,
        "a ray beside the globe cannot select a hidden province");

  std::cout << std::format("PF10 verify: {}/{} passed\n", passed, passed + failed);
  return failed == 0 ? 0 : 1;
}

} // namespace

int main(const int argc, const char** argv) {
  devils_engine::pf10::viewer_options options;
  bool run_verify = false;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument = argv[i];
    std::string value;
    if (argument == "--help" || argument == "-h") {
      usage();
      return 0;
    } else if (argument == "--verify") run_verify = true;
    else if (argument == "--view") run_verify = false;
    else if (argument == "--validation") options.validation = true;
    else if (argument == "--fixed-rotation") options.fixed_rotation = true;
    else if (prefixed(argument, "--frames=", value)) options.frames = uint32_t(std::stoul(value));
    else if (prefixed(argument, "--shot=", value)) options.dump_path = value;
    else if (prefixed(argument, "--mesh=", value)) options.mesh_side = std::clamp(uint32_t(std::stoul(value)), 32u, 512u);
    else {
      std::cerr << "unknown option: " << argument << '\n';
      usage();
      return 2;
    }
  }
  return run_verify ? verify() : devils_engine::pf10::run_viewer(options);
}
