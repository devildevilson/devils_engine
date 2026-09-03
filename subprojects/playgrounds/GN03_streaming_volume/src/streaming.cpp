#include "streaming.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <iterator>
#include <tuple>

#include "devils_engine/utils/core.h"

namespace devils_engine::gn03 {

namespace {

// Состояние чанка в глазах стримера. Присутствие («лежит в арене») от «посчитан» отделено
// намеренно: между ними чанк может не поместиться в арену, и тогда его надо считать заново.
enum class chunk_state : uint8_t { pending, in_flight, ready, present };

uint64_t mix(uint64_t value) noexcept {
  value += 0x9e3779b97f4a7c15ull;
  value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ull;
  value = (value ^ (value >> 27)) * 0x94d049bb133111ebull;
  return value ^ (value >> 31);
}

int64_t squared_distance(const originator::chunk_key& a, const originator::chunk_key& b) noexcept {
  const int64_t dx = a.x - b.x;
  const int64_t dy = a.y - b.y;
  const int64_t dz = a.z - b.z;
  return dx * dx + dy * dy + dz * dz;
}

} // namespace

void rebase(local_frame& frame, const double span) noexcept {
  if (span <= 0.0) {
    return;
  }
  std::array<double*, 3> local{&frame.position.x, &frame.position.y, &frame.position.z};
  std::array<int64_t*, 3> key{&frame.key.x, &frame.key.y, &frame.key.z};
  for (uint32_t axis = 0; axis < 3; ++axis) {
    const double value = *local[axis];
    const int64_t steps = int64_t(std::floor(value / span));
    if (steps == 0) {
      continue;
    }
    *key[axis] += steps;
    // Вычитание ЦЕЛОГО числа размеров чанка: в double оно точно, пока и `span`, и произведение
    // представимы, поэтому перенос не вносит своей ошибки поверх накопленной.
    *local[axis] = value - double(steps) * span;
  }
}

glm::dvec3 absolute_position(const local_frame& frame, const double span) noexcept {
  return glm::dvec3(double(frame.key.x) * span + frame.position.x,
                    double(frame.key.y) * span + frame.position.y,
                    double(frame.key.z) * span + frame.position.z);
}

glm::vec3 chunk_offset(const originator::chunk_key& key, const originator::chunk_key& origin,
                       const double span) noexcept {
  return glm::vec3(float(double(key.x - origin.x) * span), float(double(key.y - origin.y) * span),
                   float(double(key.z - origin.z) * span));
}

bool chunk_window::contains(const originator::chunk_key& key) const noexcept {
  return std::abs(key.x - centre.x) <= horizontal_radius && std::abs(key.z - centre.z) <= horizontal_radius &&
         std::abs(key.y - centre.y) <= vertical_radius;
}

size_t chunk_window::volume() const noexcept {
  const int64_t side = horizontal_radius * 2 + 1;
  const int64_t layers = vertical_radius * 2 + 1;
  return size_t(std::max<int64_t>(side * side * layers, 0));
}

size_t chunk_key_hash::operator()(const originator::chunk_key& key) const noexcept {
  return size_t(mix(uint64_t(key.x) ^ mix(uint64_t(key.y) ^ mix(uint64_t(key.z)))));
}

size_t chunk_streamer::key_hash::operator()(const originator::chunk_key& key) const noexcept {
  return chunk_key_hash{}(key);
}

chunk_streamer::chunk_streamer(generator_factory factory, const size_t worker_count) : factory_(std::move(factory)) {
  if (!factory_) {
    utils::error{}("GN03 streamer needs a generator factory");
  }
  const size_t workers = std::max<size_t>(worker_count, 1);
  workers_.reserve(workers);
  for (size_t i = 0; i < workers; ++i) {
    workers_.emplace_back([this] { worker_loop(); });
  }
}

chunk_streamer::~chunk_streamer() {
  {
    const std::lock_guard lock(mutex_);
    stopping_ = true;
  }
  wake_.notify_all();
  for (auto& worker : workers_) {
    if (worker.joinable()) {
      worker.join();
    }
  }
}

void chunk_streamer::set_window(const chunk_window& window) {
  std::vector<originator::chunk_key> wanted;
  wanted.reserve(window.volume());
  for (int64_t y = window.centre.y - window.vertical_radius; y <= window.centre.y + window.vertical_radius; ++y) {
    for (int64_t z = window.centre.z - window.horizontal_radius; z <= window.centre.z + window.horizontal_radius; ++z) {
      for (int64_t x = window.centre.x - window.horizontal_radius; x <= window.centre.x + window.horizontal_radius;
           ++x) {
        wanted.push_back(originator::chunk_key{x, y, z});
      }
    }
  }

  const std::lock_guard lock(mutex_);
  window_ = window;

  // Что вышло за окно. Считающийся чанк не отзывается: он всё равно досчитается, а на выходе
  // проверит окно ещё раз — прерывать работу в середине дороже, чем выбросить готовый результат.
  for (auto it = state_.begin(); it != state_.end();) {
    if (window_.contains(it->first)) {
      ++it;
      continue;
    }
    const auto state = chunk_state(it->second);
    if (state == chunk_state::in_flight) {
      ++it;
      continue;
    }
    if (state == chunk_state::present || state == chunk_state::ready) {
      evicted_.push_back(it->first);
      if (state == chunk_state::present) {
        --present_;
      }
    }
    it = state_.erase(it);
  }

  // Готовые, но ещё не забранные результаты вне окна тоже выбрасываются: они уже устарели.
  for (auto it = ready_.begin(); it != ready_.end();) {
    it = window_.contains(it->key) ? std::next(it) : ready_.erase(it);
  }

  // ОЧЕРЕДЬ ПЕРЕСОБИРАЕТСЯ ЦЕЛИКОМ, и вернуть в неё надо не только новые чанки, но и те, что ЖДАЛИ
  // своей очереди в прошлом окне.
  //
  // Здесь был баг, и симптом у него был точный: при перемещении часть чанков не появлялась никогда, а
  // если улететь и вернуться — появлялась. Причина в том, что `pending_` очищался, а `state_` помнил
  // чанк как «в очереди», поэтому проверка «его ещё нет в state_» такой чанк ПРОПУСКАЛА: он оставался
  // помеченным как ждущий, но ни в одной очереди не лежал, и его никто больше не просил. Отлёт же
  // выбрасывал его из `state_` (он выходил за окно), и на возврате он заводился заново — отсюда и
  // «вернуться помогает».
  pending_.clear();
  for (const auto& key : wanted) {
    const auto found = state_.find(key);
    if (found == state_.end()) {
      pending_.push_back(key);
      state_[key] = uint8_t(chunk_state::pending);
      continue;
    }
    if (chunk_state(found->second) == chunk_state::pending) {
      pending_.push_back(key); // всё ещё нужен и всё ещё не посчитан
    }
  }

  // Ближний — в конец: рабочий поток забирает с конца, значит первым считается тот чанк, который
  // наблюдатель уже почти видит.
  const auto centre = window_.centre;
  std::sort(pending_.begin(), pending_.end(),
            [centre](const originator::chunk_key& a, const originator::chunk_key& b) {
              const int64_t first = squared_distance(a, centre);
              const int64_t second = squared_distance(b, centre);
              if (first != second) {
                return first > second;
              }
              // Устойчивый порядок при равном расстоянии: иначе набор чанков зависел бы от того,
              // как лёг std::sort, и один и тот же полёт камеры давал бы разную очередь.
              return std::tie(a.y, a.z, a.x) > std::tie(b.y, b.z, b.x);
            });

  wake_.notify_all();
}

bool chunk_streamer::next_request(originator::chunk_key& key) {
  std::unique_lock lock(mutex_);
  wake_.wait(lock, [this] { return stopping_ || !pending_.empty(); });
  if (stopping_) {
    return false;
  }
  key = pending_.back();
  pending_.pop_back();
  state_[key] = uint8_t(chunk_state::in_flight);
  ++in_flight_;
  return true;
}

void chunk_streamer::worker_loop() {
  // Тело генерации создаётся ВНУТРИ потока: у пайплайна свои буферы, у хоста скриптов свой
  // sol::state, и оба живут ровно столько, сколько живёт поток.
  chunk_generator generate;
  {
    const std::lock_guard lock(mutex_);
    generate = factory_();
  }
  if (!generate) {
    utils::error{}("GN03 streamer got an empty generator from the factory");
  }

  originator::chunk_key key{};
  chunk_mesh mesh;
  while (next_request(key)) {
    mesh.vertices.clear();
    mesh.key = key;
    generate(key, mesh);

    const std::lock_guard lock(mutex_);
    --in_flight_;
    ++totals_.generated;
    const double milliseconds = mesh.field_milliseconds + mesh.surface_milliseconds;
    totals_.last_milliseconds = milliseconds;
    totals_.total_milliseconds += milliseconds;
    totals_.largest_chunk_vertices = std::max(totals_.largest_chunk_vertices, mesh.vertices.size());

    if (!window_.contains(key)) {
      state_.erase(key); // окно ушло, пока чанк считался
      continue;
    }
    state_[key] = uint8_t(chunk_state::ready);
    ready_.push_back(std::move(mesh));
    mesh = chunk_mesh{};
  }
}

bool chunk_streamer::pop_ready(chunk_mesh& out) {
  const std::lock_guard lock(mutex_);
  if (ready_.empty()) {
    return false;
  }
  out = std::move(ready_.front());
  ready_.pop_front();
  const auto found = state_.find(out.key);
  if (found != state_.end()) {
    found->second = uint8_t(chunk_state::present);
    ++present_;
  }
  return true;
}

std::vector<originator::chunk_key> chunk_streamer::take_evicted() {
  const std::lock_guard lock(mutex_);
  std::vector<originator::chunk_key> taken;
  taken.swap(evicted_);
  return taken;
}

void chunk_streamer::forget(const originator::chunk_key& key) {
  const std::lock_guard lock(mutex_);
  const auto found = state_.find(key);
  if (found == state_.end()) {
    return;
  }
  if (chunk_state(found->second) == chunk_state::present) {
    --present_;
  }
  state_.erase(found);
  if (window_.contains(key)) {
    pending_.push_back(key);
    state_[key] = uint8_t(chunk_state::pending);
    // РАЗБУДИТЬ ОБЯЗАТЕЛЬНО. Это второй источник того же симптома, что и потерянная очередь: если
    // рабочие потоки уже спят (очередь была пуста), возвращённый чанк лежит в ней незамеченным до
    // следующей смены окна — то есть до того, как камера перейдёт в другой чанк. Снаружи это
    // выглядит так же: чанка нет, а «улететь и вернуться» помогает.
    wake_.notify_one();
  }
}

streamer_stats chunk_streamer::stats() const {
  const std::lock_guard lock(mutex_);
  streamer_stats result = totals_;
  result.pending = pending_.size();
  result.in_flight = in_flight_;
  result.present = present_;
  return result;
}

size_t vertex_arena::key_hash::operator()(const originator::chunk_key& key) const noexcept {
  return chunk_key_hash{}(key);
}

vertex_arena::vertex_arena(const size_t capacity, const size_t granularity, const size_t retirement_frames) :
  capacity_(capacity), granularity_(std::max<size_t>(granularity, 3)), retirement_frames_(retirement_frames) {
  // Гранулярность кратна трём, потому что единица геометрии — треугольник: отрезок, не кратный
  // трём, разрезал бы треугольник между чанками, и в дырке появился бы треугольник из чужих вершин.
  granularity_ = (granularity_ + 2) / 3 * 3;
  mirror_.assign(capacity_, gpu_vertex{});

  // Слоты выдаются с конца списка, поэтому первый чанк получает нулевой: у пустой арены таблица
  // смещений заполняется с начала, и смотреть её в отладчике проще.
  free_slots_.reserve(max_chunk_slots);
  for (size_t i = max_chunk_slots; i > 0; --i) {
    free_slots_.push_back(uint32_t(i - 1));
  }
}

size_t vertex_arena::rounded(const size_t count) const noexcept {
  return (count + granularity_ - 1) / granularity_ * granularity_;
}

void vertex_arena::fill_degenerate(const block& region) {
  if (region.count == 0) {
    return;
  }
  // Вырожденный треугольник — три совпавшие вершины. Нулевая вершина именно такова, и растеризатор
  // её отбрасывает по нулевой площади, то есть дырка в арене не стоит ни одного пикселя.
  std::memset(mirror_.data() + region.first, 0, region.count * sizeof(gpu_vertex));
  dirty_.push_back(region);
}

void vertex_arena::recompute_high_water() noexcept {
  size_t water = 0;
  for (const auto& [key, held] : blocks_) {
    water = std::max(water, held.region.first + held.region.count);
  }
  high_water_ = water;
}

bool vertex_arena::insert(const originator::chunk_key& key, const std::span<const gpu_vertex> vertices) {
  if (blocks_.find(key) != blocks_.end()) {
    remove(key);
  }
  if (vertices.empty()) {
    return true; // пустой чанк занимает ноль байт и остаётся отсутствующим в арене
  }
  if (free_slots_.empty()) {
    return false; // слоты таблицы смещений кончились: это тот же отказ, что и «нет места»
  }

  const size_t needed = rounded(vertices.size());
  block chosen{0, 0};

  // Лучшая подходящая дырка, а не первая: при первой подходящей крупные дырки крошатся мелкими
  // чанками, и арена перестаёт принимать большие через несколько минут полёта.
  size_t best = free_list_.size();
  for (size_t i = 0; i < free_list_.size(); ++i) {
    if (free_list_[i].count < needed) {
      continue;
    }
    if (best == free_list_.size() || free_list_[i].count < free_list_[best].count) {
      best = i;
    }
  }

  if (best != free_list_.size()) {
    chosen = block{free_list_[best].first, needed};
    const size_t remainder = free_list_[best].count - needed;
    if (remainder == 0) {
      free_list_.erase(free_list_.begin() + std::ptrdiff_t(best));
    } else {
      free_list_[best] = block{free_list_[best].first + needed, remainder};
    }
  } else if (top_ + needed <= capacity_) {
    chosen = block{top_, needed};
    top_ += needed;
  } else {
    return false;
  }

  const uint32_t slot = free_slots_.back();
  free_slots_.pop_back();

  std::memcpy(mirror_.data() + chosen.first, vertices.data(), vertices.size() * sizeof(gpu_vertex));
  // Номер слота проставляется каждой вершине здесь: до вставки геометрия своего слота не знает, а
  // копирование всё равно идёт, поэтому лишнего обхода не появляется.
  for (size_t i = 0; i < vertices.size(); ++i) {
    mirror_[chosen.first + i].chunk = uint16_t(slot);
  }
  if (needed > vertices.size()) {
    std::memset(mirror_.data() + chosen.first + vertices.size(), 0,
                (needed - vertices.size()) * sizeof(gpu_vertex));
  }
  dirty_.push_back(chosen);

  blocks_[key] = entry{chosen, slot};
  live_.push_back(live_chunk{key, slot});
  // Учёт идёт по ВЫДАННЫМ вершинам, а не по занятым геометрией: занятость арены — это про место,
  // а место занимает отрезок целиком, вместе с округлением до гранулярности.
  used_ += chosen.count;
  high_water_ = std::max(high_water_, chosen.first + chosen.count);
  return true;
}

void vertex_arena::remove(const originator::chunk_key& key) {
  const auto found = blocks_.find(key);
  if (found == blocks_.end()) {
    return;
  }
  const block region = found->second.region;
  const uint32_t slot = found->second.slot;
  used_ = used_ > region.count ? used_ - region.count : 0;
  blocks_.erase(found);

  // ОТРЕЗОК ОБНУЛЯЕТСЯ СРАЗУ, а в оборот возвращается позже, и порядок здесь важен.
  //
  // Первая версия делала наоборот — держала байты как есть и обнуляла их при возврате места, чтобы
  // не писать в то, что читает устройство. Но слот чанка возвращался сразу, а лежащая геометрия
  // продолжала рисоваться со своим прежним номером слота: стоило новому чанку занять этот слот, и
  // старые треугольники уезжали в ЧУЖОЕ место мира на кадр-два. Обнуление сразу убирает это по
  // построению: вырожденный треугольник не рисуется, каким бы ни был его слот.
  //
  // Гонка с устройством при этом остаётся, но её последствие меняет знак: порванная запись НУЛЕЙ
  // означает, что часть треугольников исчезла кадром раньше, а порванная запись новых вершин
  // означала бы смесь двух чанков в одном треугольнике.
  fill_degenerate(region);
  free_slots_.push_back(slot);
  for (auto it = live_.begin(); it != live_.end(); ++it) {
    if (it->key == key) {
      *it = live_.back();
      live_.pop_back();
      break;
    }
  }

  retiring_.push_back(retiring{region, retirement_frames_});
  recompute_high_water();
}

void vertex_arena::advance_frame() {
  for (auto it = retiring_.begin(); it != retiring_.end();) {
    if (it->frames_left > 0) {
      --it->frames_left;
      ++it;
      continue;
    }

    // Обнулён отрезок был ещё при выгрузке; здесь он только возвращается в оборот. Ждать нужно
    // именно ПЕРЕИСПОЛЬЗОВАНИЯ: пока кадры в полёте читают эти байты, писать в них новую геометрию
    // означало бы смесь двух чанков в одном треугольнике.
    free_list_.push_back(it->region);
    it = retiring_.erase(it);
  }

  // Склейка соседних дырок. Без неё арена дробится: чанк уходит и приходит десятки раз за полёт, и
  // каждый раз оставляет отрезок своего размера.
  if (free_list_.size() > 1) {
    std::sort(free_list_.begin(), free_list_.end(),
              [](const block& a, const block& b) { return a.first < b.first; });
    std::vector<block> merged;
    merged.reserve(free_list_.size());
    for (const auto& region : free_list_) {
      if (!merged.empty() && merged.back().first + merged.back().count == region.first) {
        merged.back().count += region.count;
        continue;
      }
      merged.push_back(region);
    }
    // Дырка, доросшая до вершины арены, возвращает границу выдачи: иначе арена «кончается», хотя в
    // ней пусто.
    if (!merged.empty() && merged.back().first + merged.back().count == top_) {
      top_ = merged.back().first;
      merged.pop_back();
    }
    free_list_ = std::move(merged);
  }
}

size_t vertex_arena::hole_vertices() const noexcept {
  size_t total = 0;
  for (const auto& region : free_list_) {
    total += region.count;
  }
  for (const auto& region : retiring_) {
    total += region.region.count;
  }
  return total;
}

std::vector<vertex_arena::block> vertex_arena::take_dirty() {
  std::vector<block> taken;
  taken.swap(dirty_);
  return taken;
}

} // namespace devils_engine::gn03
