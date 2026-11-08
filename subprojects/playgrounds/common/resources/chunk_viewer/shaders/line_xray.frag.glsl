#version 450

// Та же линия сквозь стены, но тусклее: видно, куда ведёт связь за скалой, и не спутать её с той,
// что лежит в открытой воде перед камерой.

layout(location = 0) in vec4 in_colour;
layout(location = 0) out vec4 out_colour;

void main() { out_colour = vec4(in_colour.rgb * 0.35, 1.0); }
