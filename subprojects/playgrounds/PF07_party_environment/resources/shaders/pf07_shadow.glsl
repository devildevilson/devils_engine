// Общее объявление каскадов и выборка тени. Одно место на проход построения и на оба потребителя —
// геометрию и аналитическую землю: разойдись раскладка записи, и тень поехала бы относительно света
// без единого предупреждения от компилятора.

#ifndef PF07_SHADOW_GLSL
#define PF07_SHADOW_GLSL

#define PF07_SHADOW_SOURCES 2
#define PF07_CASCADE_COUNT 3

struct pf07_cascade {
  mat4 light_view_projection;
  vec4 split_depths;     // x/y — интервал в метрах, z — начало смешивания
  vec4 shadow_params;    // x — мировой размер текселя, y — сила тени источника
  vec4 uv_scale_offset;  // регион атласа как данные: local * scale + offset
};

#endif
