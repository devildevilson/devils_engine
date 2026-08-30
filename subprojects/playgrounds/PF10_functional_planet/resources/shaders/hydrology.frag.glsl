#version 450

layout(location = 0) in vec2 in_feature_coordinate;
layout(location = 1) in float in_half_width;
layout(location = 2) flat in float in_kind;
layout(location = 3) flat in float in_variant;
layout(location = 0) out vec4 out_color;

void main() {
  const float distance_to_feature = in_kind > 0.5 ? length(in_feature_coordinate) :
                                                    abs(in_feature_coordinate.y);
  const float aa = max(fwidth(distance_to_feature), 1e-6);
  const float coverage = 1.0 - smoothstep(in_half_width - aa, in_half_width + aa, distance_to_feature);
  if (coverage <= 1.0 / 255.0) discard;
  const float variant = in_variant * 0.025;
  const vec3 colour = vec3(0.035 + variant, 0.28 + variant, 0.43 + variant);
  out_color = vec4(colour, coverage * 0.94);
}
