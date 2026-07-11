#version 450

layout(location = 0) in vec2 inPosition; // local quad corner, -0.5..0.5
layout(location = 1) in vec2 inUV;

layout(location = 0) out vec2 fragUV;

layout(binding = 1) uniform CameraUBO {
    mat4 view;
    mat4 proj;
} camera;

layout(push_constant) uniform PushConstants {
    vec3 player_pos;
    float is_local;
} pc;

void main() {
    // Billboard: extract the camera's world-space right/up axes from the
    // VIEW matrix's ROWS, not columns. The view matrix is the inverse
    // (transpose, for the rotational part) of the camera's own world-space
    // orientation, so its rows hold the camera's right/up/-forward axes. In
    // GLSL's column-major mat[col][row] indexing, reading a row means
    // fixing the row index and varying the column index.
    vec3 cameraRight = vec3(camera.view[0][0], camera.view[1][0], camera.view[2][0]);
    vec3 cameraUp    = vec3(camera.view[0][1], camera.view[1][1], camera.view[2][1]);

    vec3 worldPos = pc.player_pos
                  + cameraRight * inPosition.x
                  + cameraUp    * inPosition.y;

    gl_Position = camera.proj * camera.view * vec4(worldPos, 1.0);
    fragUV = inUV;
}
