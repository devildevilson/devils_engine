#version 450

// Линия без тумана: это диагностика, а не часть мира, и смысл её — показать связи дальше, чем
// видно глазу.

layout(location = 0) in vec4 in_colour;
layout(location = 0) out vec4 out_colour;

void main() { out_colour = vec4(in_colour.rgb, 1.0); }
