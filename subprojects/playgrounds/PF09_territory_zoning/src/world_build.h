#ifndef DEVILS_ENGINE_PF09_WORLD_BUILD_H
#define DEVILS_ENGINE_PF09_WORLD_BUILD_H

// Сборка фикстуры мира в секторные файлы. Это не часть рантайма: игра такие файлы читает, а делает их
// редактор или генератор. Здесь она нужна ровно затем, чтобы стриминг проверялся на настоящих файлах на
// диске, а не на подделке в памяти — разница между ними и есть то, что ломается в реальных проектах.

#include <cstdint>
#include <filesystem>

#include "locality.h"
#include "territory.h"
#include "zones.h"

namespace devils_engine::pf09 {

struct build_options {
  std::filesystem::path root;
  int32_t sector_x = 60;   // левый нижний сектор области сборки
  int32_t sector_y = 60;
  uint32_t sector_side = 6; // сторона квадрата секторов
};

struct build_stats {
  uint32_t sectors = 0;
  uint32_t zones = 0;
  uint32_t links = 0;
  uint32_t settlements = 0;
  uint64_t bytes = 0;
  double millis = 0.0;
};

build_stats build_world(const territory& map, const locality_config& local, const build_options& options);

} // namespace devils_engine::pf09

#endif
