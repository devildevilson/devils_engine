#include "skeleton.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "devils_engine/utils/core.h"
#include "devils_engine/utils/fileio.h"

namespace devils_engine::gn03 {

namespace {

constexpr uint32_t file_magic = 0x334b5347u; // "GSK3"
constexpr uint32_t file_version = 1;

struct file_header {
  uint32_t magic = file_magic;
  uint32_t version = file_version;
  uint64_t seed = 0;
  double world_span = 0.0;
  double influence = 0.0;
  uint64_t node_count = 0;
  uint64_t point_count = 0;
  uint64_t offset_count = 0;
};

// Сторона тайла индекса. Больше поля запроса намеренно: тайл меньше поля означал бы, что один чанк
// всегда трогает четыре тайла, и индекс не экономил бы ничего.
constexpr double default_tile_size = 256.0;

} // namespace

void world_skeleton::build(const description& what, std::vector<skeleton_node> nodes,
                           std::vector<std::array<double, 3>> points, std::vector<uint32_t> offsets) {
  if (offsets.size() < 2) {
    utils::error{}("GN03 skeleton needs at least two CSR offsets, got {}", offsets.size());
  }
  for (size_t i = 1; i < offsets.size(); ++i) {
    if (offsets[i] < offsets[i - 1]) {
      utils::error{}("GN03 skeleton offsets go backwards at {}: {} after {}", i, offsets[i], offsets[i - 1]);
    }
  }
  if (offsets.back() > points.size()) {
    utils::error{}("GN03 skeleton says {} points but the buffer holds {}", offsets.back(), points.size());
  }

  description_ = what;
  nodes_ = std::move(nodes);
  points_ = std::move(points);
  offsets_ = std::move(offsets);
  build_index();
}

void world_skeleton::build_index() {
  segments_.clear();
  tiles_.clear();

  for (size_t chain = 0; chain + 1 < offsets_.size(); ++chain) {
    const size_t first = offsets_[chain];
    const size_t last = offsets_[chain + 1];
    for (size_t i = first; i + 1 < last; ++i) {
      segments_.push_back(segment{points_[i], points_[i + 1], uint32_t(chain), uint32_t(i)});
    }
  }

  if (segments_.empty()) {
    tile_size_ = default_tile_size;
    tile_count_[0] = tile_count_[1] = 0;
    return;
  }

  tile_size_ = default_tile_size;
  double low[2]{segments_.front().from[0], segments_.front().from[2]};
  double high[2]{low[0], low[1]};
  for (const auto& piece : segments_) {
    for (uint32_t axis = 0; axis < 2; ++axis) {
      const size_t component = axis == 0 ? 0 : 2;
      low[axis] = std::min({low[axis], piece.from[component], piece.to[component]});
      high[axis] = std::max({high[axis], piece.from[component], piece.to[component]});
    }
  }

  // Границы индекса расширяются на поле запроса: отрезок у самого края мира всё равно обязан
  // находиться запросом чанка, который лежит ЗА этим краем.
  const double margin = description_.influence + tile_size_;
  for (uint32_t axis = 0; axis < 2; ++axis) {
    tile_low_[axis] = int64_t(std::floor((low[axis] - margin) / tile_size_));
    const int64_t top = int64_t(std::floor((high[axis] + margin) / tile_size_));
    tile_count_[axis] = top - tile_low_[axis] + 1;
  }

  tiles_.assign(size_t(tile_count_[0] * tile_count_[1]), {});
  for (size_t index = 0; index < segments_.size(); ++index) {
    const auto& piece = segments_[index];
    // Отрезок кладётся во ВСЕ тайлы, которых касается его прямоугольник, расширенный радиусом
    // влияния. Иначе запрос по тайлу чанка не нашёл бы отрезок, проходящий рядом с ним — то есть
    // индекс был бы неполным, а это другой мир.
    double box_low[2]{};
    double box_high[2]{};
    for (uint32_t axis = 0; axis < 2; ++axis) {
      const size_t component = axis == 0 ? 0 : 2;
      box_low[axis] = std::min(piece.from[component], piece.to[component]) - description_.influence;
      box_high[axis] = std::max(piece.from[component], piece.to[component]) + description_.influence;
    }

    const int64_t from_x = std::max(int64_t(std::floor(box_low[0] / tile_size_)), tile_low_[0]);
    const int64_t to_x = std::min(int64_t(std::floor(box_high[0] / tile_size_)), tile_low_[0] + tile_count_[0] - 1);
    const int64_t from_z = std::max(int64_t(std::floor(box_low[1] / tile_size_)), tile_low_[1]);
    const int64_t to_z = std::min(int64_t(std::floor(box_high[1] / tile_size_)), tile_low_[1] + tile_count_[1] - 1);
    for (int64_t z = from_z; z <= to_z; ++z) {
      for (int64_t x = from_x; x <= to_x; ++x) {
        const size_t tile = size_t((z - tile_low_[1]) * tile_count_[0] + (x - tile_low_[0]));
        tiles_[tile].push_back(index);
      }
    }
  }
}

bool world_skeleton::collect(const std::array<double, 3>& low, const std::array<double, 3>& high,
                             const std::span<const size_t> candidates, const size_t point_capacity,
                             const size_t chain_capacity, query_result& out) const {
  const double margin = description_.influence;

  // Отобранные отрезки складываются в НЕПРЕРЫВНЫЕ подцепочки: два соседних отрезка одной цепочки
  // обязаны остаться соединёнными, иначе между ними появится разрыв коридора там, где маршрут ровный.
  std::vector<size_t> chosen;
  chosen.reserve(candidates.size());
  for (const size_t index : candidates) {
    const auto& piece = segments_[index];
    bool touches = true;
    for (uint32_t axis = 0; axis < 3; ++axis) {
      const double piece_low = std::min(piece.from[axis], piece.to[axis]) - margin;
      const double piece_high = std::max(piece.from[axis], piece.to[axis]) + margin;
      touches = touches && piece_high >= low[axis] && piece_low <= high[axis];
    }
    if (touches) {
      chosen.push_back(index);
    }
  }
  std::sort(chosen.begin(), chosen.end());
  chosen.erase(std::unique(chosen.begin(), chosen.end()), chosen.end());

  out.points.clear();
  out.offsets.clear();
  out.chains = 0;
  out.offsets.push_back(0);

  size_t i = 0;
  while (i < chosen.size()) {
    const auto& head = segments_[chosen[i]];
    size_t j = i + 1;
    while (j < chosen.size() && segments_[chosen[j]].chain == head.chain &&
           segments_[chosen[j]].point == segments_[chosen[j - 1]].point + 1) {
      ++j;
    }

    // Подцепочка из (j - i) отрезков — это (j - i + 1) точек.
    const size_t needed = j - i + 1;
    if (out.points.size() + needed > point_capacity || out.chains + 1 >= chain_capacity) {
      return false;
    }
    out.points.push_back(segments_[chosen[i]].from);
    for (size_t k = i; k < j; ++k) {
      out.points.push_back(segments_[chosen[k]].to);
    }
    out.offsets.push_back(uint32_t(out.points.size()));
    out.chains += 1;
    i = j;
  }

  return true;
}

bool world_skeleton::query(const std::array<double, 3>& low, const std::array<double, 3>& high,
                           const size_t point_capacity, const size_t chain_capacity, query_result& out) const {
  if (segments_.empty()) {
    out.points.clear();
    out.offsets.assign(1, 0);
    out.chains = 0;
    return true;
  }

  // Тайлы, которых касается область чанка, расширенная радиусом влияния. Поле прибавляется ЗДЕСЬ, а
  // не вызывающим: забыть его — значит потерять отрезок, проходящий рядом с чанком.
  const double margin = description_.influence;
  std::vector<size_t> candidates;
  const int64_t from_x = int64_t(std::floor((low[0] - margin) / tile_size_));
  const int64_t to_x = int64_t(std::floor((high[0] + margin) / tile_size_));
  const int64_t from_z = int64_t(std::floor((low[2] - margin) / tile_size_));
  const int64_t to_z = int64_t(std::floor((high[2] + margin) / tile_size_));
  for (int64_t z = from_z; z <= to_z; ++z) {
    if (z < tile_low_[1] || z >= tile_low_[1] + tile_count_[1]) {
      continue;
    }
    for (int64_t x = from_x; x <= to_x; ++x) {
      if (x < tile_low_[0] || x >= tile_low_[0] + tile_count_[0]) {
        continue;
      }
      const size_t tile = size_t((z - tile_low_[1]) * tile_count_[0] + (x - tile_low_[0]));
      candidates.insert(candidates.end(), tiles_[tile].begin(), tiles_[tile].end());
    }
  }

  return collect(low, high, candidates, point_capacity, chain_capacity, out);
}

bool world_skeleton::query_exhaustive(const std::array<double, 3>& low, const std::array<double, 3>& high,
                                      const size_t point_capacity, const size_t chain_capacity,
                                      query_result& out) const {
  std::vector<size_t> everything(segments_.size());
  for (size_t i = 0; i < segments_.size(); ++i) {
    everything[i] = i;
  }
  return collect(low, high, everything, point_capacity, chain_capacity, out);
}

bool world_skeleton::save(const std::string& path) const {
  file_header header;
  header.seed = description_.seed;
  header.world_span = description_.world_span;
  header.influence = description_.influence;
  header.node_count = nodes_.size();
  header.point_count = points_.size();
  header.offset_count = offsets_.size();

  std::vector<char> bytes(sizeof(header) + nodes_.size() * sizeof(skeleton_node) +
                          points_.size() * sizeof(std::array<double, 3>) + offsets_.size() * sizeof(uint32_t));
  size_t cursor = 0;
  const auto append = [&bytes, &cursor](const void* data, const size_t size) {
    if (size != 0) {
      std::memcpy(bytes.data() + cursor, data, size);
      cursor += size;
    }
  };
  append(&header, sizeof(header));
  append(nodes_.data(), nodes_.size() * sizeof(skeleton_node));
  append(points_.data(), points_.size() * sizeof(std::array<double, 3>));
  append(offsets_.data(), offsets_.size() * sizeof(uint32_t));

  return file_io::write(std::span<const char>(bytes.data(), bytes.size()), path, file_io::type::binary);
}

bool world_skeleton::load(const std::string& path) {
  const auto bytes = file_io::read<char>(path, file_io::type::binary);
  if (bytes.empty()) {
    return false;
  }
  if (bytes.size() < sizeof(file_header)) {
    utils::error{}("GN03 skeleton '{}' is {} bytes, smaller than its own header", path, bytes.size());
  }

  file_header header;
  std::memcpy(&header, bytes.data(), sizeof(header));
  if (header.magic != file_magic) {
    utils::error{}("GN03 skeleton '{}' does not start with the expected mark", path);
  }
  if (header.version != file_version) {
    utils::error{}("GN03 skeleton '{}' is version {}, and this build reads version {}", path, header.version,
                   file_version);
  }

  const size_t expected = sizeof(file_header) + size_t(header.node_count) * sizeof(skeleton_node) +
                          size_t(header.point_count) * sizeof(std::array<double, 3>) +
                          size_t(header.offset_count) * sizeof(uint32_t);
  if (bytes.size() != expected) {
    utils::error{}("GN03 skeleton '{}' claims {} nodes, {} points and {} offsets, which needs {} bytes, but the "
                   "file is {}",
                   path, header.node_count, header.point_count, header.offset_count, expected, bytes.size());
  }

  description_.seed = header.seed;
  description_.world_span = header.world_span;
  description_.influence = header.influence;

  nodes_.resize(size_t(header.node_count));
  points_.resize(size_t(header.point_count));
  offsets_.resize(size_t(header.offset_count));

  size_t cursor = sizeof(file_header);
  const auto take = [&bytes, &cursor](void* data, const size_t size) {
    if (size != 0) {
      std::memcpy(data, bytes.data() + cursor, size);
      cursor += size;
    }
  };
  take(nodes_.data(), nodes_.size() * sizeof(skeleton_node));
  take(points_.data(), points_.size() * sizeof(std::array<double, 3>));
  take(offsets_.data(), offsets_.size() * sizeof(uint32_t));

  build_index();
  return true;
}

} // namespace devils_engine::gn03
