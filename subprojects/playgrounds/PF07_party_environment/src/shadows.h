#ifndef DEVILS_ENGINE_PF07_SHADOWS_H
#define DEVILS_ENGINE_PF07_SHADOWS_H

// Тени двух светил, срез 4.
//
// Ключевое решение: система теней НЕ знает про «солнца» и «луны». Она берёт два самых ярких источника
// на небе, какими бы они ни были, и раздаёт им каскады. Днём это `Aurin` и `Ember`, в сумерках может
// оказаться `Ember` и `Selen`, ночью — две луны. Одно правило вместо трёх режимов, и передача
// главенства получается сама собой, а не отдельной веткой кода.
//
// Почему это безопасно при СМЕНЕ состава: пока оба слота заняты и вклад каждого умножается на его
// долю света, перестановка тел между слотами не меняет вообще ничего. Щелчок возможен только когда
// тело ВХОДИТ в двойку или выходит из неё, поэтому у порога сила тени гасится плавно.

#include <array>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include <glm/mat4x4.hpp>
#include <glm/vec3.hpp>
#include <glm/vec4.hpp>

#include "devils_engine/painter/atlas_layout.h"
#include "devils_engine/playground/free_camera.h"

#include "celestial.h"

namespace devils_engine::pf07 {

// Два источника с тенями и три каскада на каждый. Три, а не четыре: сцена среза заканчивается на
// пятидесяти метрах, и четвёртый каскад пришлось бы растягивать на пустую землю.
constexpr uint32_t shadow_source_count = 2;
constexpr uint32_t cascade_count = 3;
constexpr uint32_t cascade_total = shadow_source_count * cascade_count;
constexpr uint32_t cascade_tile_size = 1024;

// Запись каскада для видеокарты. Раскладка региона в атласе передаётся ДАННЫМИ, поэтому шейдер не
// знает, как атлас устроен, и число каскадов можно менять, не трогая GLSL.
struct alignas(16) cascade_record {
  glm::mat4 light_view_projection{1.0f};
  glm::vec4 split_depths{0.0f};      // x/y — интервал в метрах, z — начало смешивания, w — резерв
  glm::vec4 shadow_params{0.0f};     // x — мировой размер текселя, y — сила тени источника
  glm::vec4 uv_scale_offset{1.0f, 1.0f, 0.0f, 0.0f};
};
static_assert(sizeof(cascade_record) == 7 * sizeof(glm::vec4));

// Источник, получивший каскады.
struct shadow_source {
  glm::vec3 direction{0.0f, 1.0f, 0.0f};   // от поверхности К источнику, мировые оси
  glm::vec3 color{1.0f};
  double illuminance_lx = 0.0;
  double strength = 0.0;                   // 0..1, гасится у порога вхождения в двойку
  std::string_view name;
  // Код тела для блока неба: 0..1 — звезда с этим индексом, 2+m — луна m. Освещение поверхности
  // иначе не поймёт, чья это тень: система теней берёт два самых ярких источника, кем бы они ни были.
  float body_code = -1.0f;
  bool active = false;
};

using shadow_source_set = std::array<shadow_source, shadow_source_count>;

// Два самых ярких источника над горизонтом. Возвращает их в порядке убывания освещённости.
shadow_source_set select_shadow_sources(const sky_state& state);

// Каскады для одного источника. Записывает `cascade_count` подряд идущих записей.
//
// `caster_height` — самый высокий предмет сцены. Он задаёт запас каскада вдоль луча света, и без него
// низкое светило теряет дальнюю часть каждой тени: чтобы кастер попал в карту, нужно
// `caster_height / sin(высоты светила)`, а это сотни метров уже при пяти градусах.
void build_cascades(const shadow_source& source, const playground::free_camera& camera, float aspect,
                    float vertical_fov, float camera_near, float shadow_far, float caster_height,
                    std::span<const painter::atlas_region> regions, uint32_t atlas_width,
                    uint32_t atlas_height, std::span<cascade_record> out);

} // namespace devils_engine::pf07

#endif
