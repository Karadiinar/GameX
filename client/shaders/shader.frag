#version 450

layout(location = 0) in vec2 fragUV;
layout(location = 0) out vec4 outColor;

layout(binding = 0) uniform sampler2D texSampler;

layout(push_constant) uniform PushConstants {
    vec3 player_pos; // unused here, but must match the vertex shader's block byte-for-byte
    float is_local;
} pc;

void main() {
    vec4 texColor = texture(texSampler, fragUV);
    vec3 remoteTint = vec3(1.0, 0.4, 0.4);
    vec3 tinted = mix(texColor.rgb * remoteTint, texColor.rgb, pc.is_local);
    outColor = vec4(tinted, texColor.a);
}
