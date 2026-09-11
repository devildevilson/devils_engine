#include "core/terrain.h"

#include <devils_engine/originator/generator_resource.h>
#include <devils_engine/originator/pipeline.h>
#include <devils_engine/originator/primitives.h>
#include <devils_engine/originator/script_host.h>
#include <devils_engine/originator/tools.h>
#include <devils_engine/utils/core.h>
#include <devils_engine/utils/prng.h> // utils::mix

namespace frontier_online {
namespace core {

namespace originator = devils_engine::originator;
namespace demiurg = devils_engine::demiurg;
namespace utils = devils_engine::utils;
namespace thread = devils_engine::thread;

namespace {

// Имя константы размера, которой буферы генератора объявляют свою форму. Генератор обязан назвать
// свою стоимость по памяти до запуска; конкретное число подставляет хост, потому что размер чанка
// принадлежит миру, а не генератору.
constexpr std::string_view chunk_side_constant = "chunk_side";
constexpr std::string_view tiles_buffer = "tiles";
constexpr std::string_view terrain_field = "terrain";

} // namespace

void tile_grid::resize(const uint32_t w, const uint32_t h) {
  width = w;
  height = h;
  tiles.assign(size_t(w) * size_t(h), tile{});
}

void apply_chunk(tile_grid& grid, const tile_chunk& chunk) {
  if (!chunk.valid() || grid.width == 0 || grid.height == 0) {
    return;
  }

  const int32_t base_x = chunk.coord.x * int32_t(chunk.size);
  const int32_t base_y = chunk.coord.y * int32_t(chunk.size);

  for (uint32_t y = 0; y < chunk.size; ++y) {
    const int32_t gy = base_y + int32_t(y);
    if (gy < 0 || gy >= int32_t(grid.height)) {
      continue;
    }

    for (uint32_t x = 0; x < chunk.size; ++x) {
      const int32_t gx = base_x + int32_t(x);
      if (gx < 0 || gx >= int32_t(grid.width)) {
        continue;
      }
      grid.at(uint32_t(gx), uint32_t(gy)) = chunk.at(x, y);
    }
  }
}

// Всё, что нужно держать живым между чанками. Лежит за pimpl, чтобы причинный заголовок не тянул
// за собой ни sol, ни устройство пайплайна: земля — это контракт «ключ → клетки», а не originator.
struct terrain_source::state {
  // Реестр инструментов обязан быть ЗАПОЛНЕН до постройки script_host: хост снимает с него
  // окружение lua один раз, и инструмент, добавленный позже, в скрипте окажется nil. Порядок
  // членов здесь — это порядок инициализации, поэтому заполнение вписано прямо в него.
  static originator::tool_registry& filled(originator::tool_registry& registry) {
    registry.add_standard_tools();
    originator::add_all_primitives(registry);
    return registry;
  }

  originator::tool_registry tools;
  originator::generator_config package;
  originator::script_host host;
  originator::pipeline line;
  size_t terrain_field_index = 0;

  state(const demiurg::resource_system& resources, const std::string_view& entry_id,
        const uint32_t chunk_size, const uint64_t seed, thread::atomic_pool* pool,
        const originator::size_table& sizes)
    : package(originator::load_generator(resources, entry_id)),
      host(filled(tools), pool),
      line(package.description, sizes, seed) {
    for (const auto& step : package.description.steps) {
      // Имя чанка для lua — demiurg-id тела: в сообщении об ошибке скрипта стоит тот же адрес, по
      // которому этот скрипт лежит в модуле.
      host.load_body(step.name, package.source(step.body), step.body);
      for (const auto& [program_name, id] : step.programs) {
        host.load_program(program_name, package.source(id));
      }
    }

    const auto* tiles = line.find_buffer(tiles_buffer);
    if (tiles == nullptr) {
      utils::error{}("frontier_online terrain: generator '{}' declares no '{}' buffer", entry_id,
                     tiles_buffer);
    }
    if (tiles->count() != size_t(chunk_size) * chunk_size) {
      utils::error{}("frontier_online terrain: buffer '{}' holds {} elements, chunk {}x{} needs {}",
                     tiles_buffer, tiles->count(), chunk_size, chunk_size,
                     size_t(chunk_size) * chunk_size);
    }
    terrain_field_index = tiles->find_field(terrain_field);
    if (terrain_field_index == size_t(-1)) {
      utils::error{}("frontier_online terrain: buffer '{}' has no field '{}'", tiles_buffer,
                     terrain_field);
    }
  }
};

terrain_source::terrain_source(const demiurg::resource_system& resources,
                               const std::string_view entry_id, const uint32_t chunk_size,
                               const uint64_t world_seed, thread::atomic_pool* pool)
  : chunk_size_(chunk_size), world_seed_(world_seed) {
  if (chunk_size == 0) {
    utils::error{}("frontier_online terrain: chunk size must not be zero");
  }

  originator::size_table sizes;
  sizes.set(chunk_side_constant, chunk_size);

  state_ = std::make_unique<state>(resources, entry_id, chunk_size, world_seed, pool, sizes);

  // Отпечаток мира. В него входит ВСЁ, от чего зависят клетки: зерно, размер чанка и тексты всех
  // документов генератора — точка входа, значения, буферы, тела шагов. Правка порога в values.tavl
  // меняет мир ровно так же, как другое зерно, и рукопожатие обязано это заметить.
  uint64_t hash = utils::mix(world_seed, uint64_t(chunk_size));
  for (const auto& [id, text] : state_->package.sources) {
    hash = utils::mix(hash, utils::murmur_hash64A(id));
    hash = utils::mix(hash, utils::murmur_hash64A(text));
  }
  fingerprint_ = hash;
}

terrain_source::~terrain_source() noexcept = default;

tile_chunk terrain_source::generate(const chunk_coord coord) {
  tile_chunk chunk;
  chunk.coord = coord;
  chunk.size = chunk_size_;
  chunk.tiles.assign(size_t(chunk_size_) * chunk_size_, tile{});

  // Ключ чанка — единственное, что меняется между прогонами. Буферы при этом НЕ перевыделяются:
  // один пайплайн считает много чанков подряд, и это единственный способ не платить аллокацией за
  // каждый из них.
  state_->line.set_chunk(originator::chunk_key{coord.x, coord.y, 0});
  state_->line.run(state_->host.invoker());

  const auto* tiles = state_->line.find_buffer(tiles_buffer);
  const auto terrain = tiles->field(state_->terrain_field_index);
  for (size_t i = 0; i < chunk.tiles.size(); ++i) {
    chunk.tiles[i].terrain = terrain_code(terrain.get(i));
  }
  return chunk;
}

double terrain_source::raw_field(const std::string_view field_name, const size_t element) const {
  const auto* tiles = state_->line.find_buffer(tiles_buffer);
  const size_t index = tiles->find_field(field_name);
  if (index == size_t(-1)) {
    utils::error{}("frontier_online terrain: buffer '{}' has no field '{}'", tiles_buffer,
                   field_name);
  }
  return tiles->field(index).get(element);
}

} // namespace core
} // namespace frontier_online
