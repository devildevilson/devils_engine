#version 450

// Алгоритм: две пары screen-space triangles уже лежат в clip coordinates. Левая и правая пластины имеют
// противоположный framebuffer winding, поэтому один draw попадает соответственно в front и back
// VkStencilOpState. Instance XY добавляется к clip position: в fixture он равен нулю, но сохраняет обычный
// Painter draw-group контракт и не оставляет лишний vertex attribute в pipeline.

layout(location = 0) in vec2 in_position;
layout(location = 1) in vec4 in_instance;

void main() {
  gl_Position = vec4(in_position + in_instance.xy, 0.0, 1.0);
}
