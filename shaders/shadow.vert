#version 450

layout (location = 0) in vec3 v_position;

layout (push_constant) uniform ShadowPushConstants {
    mat4 light_space;
    mat4 model;
};

void main() {
    gl_Position = light_space * model * vec4(v_position, 1.0);
}
