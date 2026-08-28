// Общее объявление каскадов и выборка тени. Одно место на проход построения и потребителей сцены:
// разойдись раскладка записи, и тень поехала бы относительно света
// без единого предупреждения от компилятора.

#ifndef PF08_SHADOW_GLSL
#define PF08_SHADOW_GLSL

#define PF08_SHADOW_SOURCES 2
#define PF08_CASCADE_COUNT 3

struct pf08_cascade {
  mat4 light_view_projection;
  vec4 split_depths;     // x/y — интервал в метрах, z — начало смешивания
  vec4 shadow_params;    // x — мировой размер текселя, y — сила тени источника
  vec4 uv_scale_offset;  // регион атласа как данные: local * scale + offset
};

#endif
