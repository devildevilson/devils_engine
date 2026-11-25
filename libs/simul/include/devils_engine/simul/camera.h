#ifndef DEVILS_ENGINE_SIMUL_CAMERA_H
#define DEVILS_ENGINE_SIMUL_CAMERA_H

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include <glm/glm.hpp>

// КАМЕРА — тип в конфиге, состояние на главном потоке.
//
// Тип камеры объявляется данными (`camera_config`, tavl): проекция, контроллер, пределы, тело.
// Состояние (`camera2d`) считает главный поток шагом контроллера (`step_camera`) — там же, где
// будет физика тела, — и публикует снапшотами; рендер только смешивает два последних
// (`value_interpolator`, interpolation.h). Готовой матрицы в рендер не шлют: проекция интерфейса и
// размер кадра у рендера свои.

namespace devils_engine {
namespace simul {

// Ортографическая камера вида сверху: центр, полуширина видимой области (мировые единицы), aspect.
struct camera2d {
  glm::vec2 center{0.0f, 0.0f};
  float half_width = 8.0f;
  float aspect = 16.0f / 9.0f; // width/height вьюпорта

  float half_height() const noexcept {
    return half_width / aspect;
  }
  // Видимая область как мировой AABB. min = левый-нижний угол, max = правый-верхний.
  glm::vec2 view_min() const noexcept {
    return center - glm::vec2(half_width, half_height());
  }
  glm::vec2 view_max() const noexcept {
    return center + glm::vec2(half_width, half_height());
  }
  // Матрица world(xy) -> clip.
  glm::mat4 view_proj() const;
};

// Мировой AABB.
struct view_bounds {
  glm::vec2 min{0.0f, 0.0f};
  glm::vec2 max{0.0f, 0.0f};
};

// Камеры, которые рендер ещё может показать. Он смешивает два последних опубликованных снапшота, а
// кадры в полёте дорисовывают снапшоты ещё старше: отбор того, что рисовать (тайлы, чанки), обязан
// покрыть их все, иначе в движении на краю кадра виден пустой фон. Главный поток кладёт сюда каждую
// опубликованную камеру и отбирает по bounds(текущая).
class published_cameras {
public:
  static constexpr size_t depth = 3;

  void push(const camera2d& camera) noexcept {
    for (size_t i = depth - 1; i > 0; --i) {
      cameras_[i] = cameras_[i - 1];
    }
    cameras_[0] = camera;
    count_ = count_ < depth ? count_ + 1 : count_;
  }
  void clear() noexcept {
    count_ = 0;
  }

  // AABB видимых областей current и опубликованных камер. Камера дальше двух полуширин от текущей —
  // прыжок (новый мир, сброс в центр), а не движение: не входит, иначе раздула бы область на полкарты.
  view_bounds bounds(const camera2d& current) const noexcept {
    view_bounds out{current.view_min(), current.view_max()};
    for (size_t i = 0; i < count_; ++i) {
      const camera2d& seen = cameras_[i];
      if (glm::distance(seen.center, current.center) > 2.0f * current.half_width) {
        continue;
      }
      out.min = glm::min(out.min, seen.view_min());
      out.max = glm::max(out.max, seen.view_max());
    }
    return out;
  }

private:
  std::array<camera2d, depth> cameras_{};
  size_t count_ = 0;
};

// Window-space cursor (origin top-left) -> world point. Логические размеры окна, не пиксели
// фреймбуфера: HiDPI-курсор остаётся согласованным.
inline glm::vec2 screen_to_world(const camera2d& cam, const glm::vec2 cursor, const glm::vec2 window_size) noexcept {
  const glm::vec2 safe_size = glm::max(window_size, glm::vec2{1.0f, 1.0f});
  const glm::vec2 unit = glm::clamp(cursor / safe_size, glm::vec2{0.0f}, glm::vec2{1.0f});
  return cam.view_min() + unit * (cam.view_max() - cam.view_min());
}

// Тип камеры. Все поля — слова и числа конфига; неизвестное слово — ошибка при загрузке
// (validate_camera_config), а не тихое умолчание.
struct camera_config {
  // "orthographic" — вид сверху, видимая полуширина half_width.
  std::string projection = "orthographic";
  float half_width = 8.0f;
  // "pan" — named actions camera_up/down/left/right двигают центр; "fixed" — камера стоит.
  std::string controller = "pan";
  // Скорость pan в полуширинах экрана в секунду (масштаб-независимо).
  float pan_speed = 1.5f;
  // "world" — центр не выходит из бокса мира; "none" — без предела.
  std::string limits = "world";
  // Физическое тело камеры (скольжение по стенам). Пока только "none": тело появится вместе с
  // физикой главного потока.
  std::string body = "none";
};

// Проверить тип камеры; owner — кто объявил (для сообщения). Ошибка — utils::error.
void validate_camera_config(const camera_config& config, std::string_view owner);

// Направление pan из named actions camera_* — camera_input.h (заголовок с вводом, как game_host.h).

// Шаг контроллера камеры на главном потоке. direction — из ввода (не нормирован), dt — секунды
// кадра, [world_min, world_max] — бокс мира для limits = "world".
void step_camera(camera2d& camera, const camera_config& config, glm::vec2 direction, float dt,
                 glm::vec2 world_min, glm::vec2 world_max);

} // namespace simul
} // namespace devils_engine

#endif
