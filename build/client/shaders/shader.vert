#version 450

layout(location = 0) out vec3 fragColor;

// 1. Declare the Push Constant block as a vec2 (X and Y matched together)
layout(push_constant) uniform PushConstants {
    vec2 player_pos;
} pc;

vec2 positions[3] = vec2[](
    vec2(0.0, -0.5),
    vec2(0.5, 0.5),
    vec2(-0.5, 0.5)
);

vec3 colors[3] = vec3[](
    vec3(1.0, 0.0, 0.0),
    vec3(0.0, 1.0, 0.0),
    vec3(0.0, 0.0, 1.0)
);

void main() {
    // 2. Add the vec2 offset directly to our base vertex coordinates!
    gl_Position = vec4(positions[gl_VertexIndex] + pc.player_pos, 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}