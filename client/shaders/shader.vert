#version 450

layout(location = 0) out vec3 fragColor;

// 1. Declare the Push Constant block
layout(push_constant) uniform PushConstants {
    float player_x;
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
    // 2. Add the player_x to the triangle's X position!
    // Note: We multiply by 0.05 because Vulkan screen space is only -1.0 to 1.0. 
    // Since your server X coordinate goes up to 60+, the triangle would instantly 
    // fly off the screen if we didn't scale it down!
    float scaled_x = pc.player_x * 0.05; 

    gl_Position = vec4(positions[gl_VertexIndex].x + scaled_x, positions[gl_VertexIndex].y, 0.0, 1.0);
    fragColor = colors[gl_VertexIndex];
}