#version 450

layout (location = 0) in vec3 v_position;
layout (location = 1) in vec3 v_normal;
layout (location = 2) in vec2 v_uv;
layout (location = 3) in vec3 v_color;

layout (location = 0) out vec3 f_position;
layout (location = 1) out vec3 f_normal;
layout (location = 2) out vec2 f_uv;
layout (location = 3) out vec3 f_color;

// NOTE: Scene uniforms (глобальные параметры)
layout (binding = 0, std140) uniform SceneUniforms {
    mat4 view_projection;
    vec3 view_position;
    float _pad0;
    vec3 ambient_light_intensity;
    float _pad1;
    vec3 sun_light_direction;
    float _pad2;
    vec3 sun_light_color;
    float _pad3;
    uint spot_lights_count;
};

// NOTE: Model uniforms (параметры по объекту)
layout (binding = 1, std140) uniform ModelUniforms {
    mat4 model;
    vec3 albedo_color;
    float _pad0_m;
    vec3 specular_color;
    float _pad1_m;
    float shininess;
    float is_skybox;
    float _pad3_m;
    float _pad4_m;
};
void main() {
    vec4 world_position = model * vec4(v_position, 1.0);
    vec4 world_normal = model * vec4(v_normal, 0.0);

    vec4 clip_pos = view_projection * world_position;

    // ============================================
    // НОВОЕ: Проверяем флаг скайбокса
    // ============================================
    if (is_skybox > 0.5) {
        // Для скайбокса: устанавливаем z = w
        gl_Position = clip_pos.xyww;
    } else {
        // Для обычных объектов: оставляем как есть
        gl_Position = clip_pos;
    }

    f_position = world_position.xyz;
    f_normal = normalize(world_normal.xyz);
    f_uv = v_uv;
    f_color = v_color;
}