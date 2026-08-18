// ЕДИНСТВЕННЫЙ источник раскладки данных, которые PF02 передаёт шейдерам.
// Раньше эти объявления были скопированы в пять шейдеров, и рассинхрон обнаруживался только по кривой
// картинке: при добавлении поля в запись каскада вертексные шейдеры остались со старым stride, из-за чего
// все элементы кроме нулевого читались со сдвигом — тени пропали на каскадах выше первого, а spot-тени
// считались по мусорным матрицам. Поля добавлять ТОЛЬКО здесь.
#ifndef PF02_RECORDS_GLSL
#define PF02_RECORDS_GLSL

// Порядок и размер должны совпадать с scene_block в src/main.cpp.
#define PF02_SCENE_BLOCK_BODY   \
  mat4 view_projection;         \
  mat4 view;                    \
  mat4 light_view_projection;   \
  vec4 camera_position;         \
  vec4 viewport_near;           \
  vec4 light_direction;         \
  vec4 shadow_params;           \
  vec4 filter_params;           \
  vec4 contact_params;          \
  vec4 shadow_layout;

// Совпадает с directional_cascade_record в src/main.cpp.
struct DirectionalCascade {
  mat4 light_view_projection;
  vec4 split_depths;
  vec4 shadow_params;
  vec4 uv_scale_offset;
};

// Совпадает с spot_light_record в src/main.cpp.
struct SpotLight {
  mat4 light_view_projection;
  vec4 position_range;
  vec4 direction_outer;
  vec4 color_intensity;
  vec4 shadow_params;
  vec4 uv_scale_offset;
};

#endif
