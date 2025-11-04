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
    float _pad2_m;
    float _pad3_m;
    float _pad4_m;
};

void main() {
    // NOTE: Преобразуем позицию в мировые координаты
    vec4 world_position = model * vec4(v_position, 1.0);

    // NOTE: Преобразуем нормаль в мировые координаты (w=0 для векторов)
    vec4 world_normal = model * vec4(v_normal, 0.0);

    // NOTE: Финальное преобразование: мир - экран (NDC)
    gl_Position = view_projection * world_position;

    // NOTE: Передаём данные во фрагментный шейдер
    f_position = world_position.xyz;
    f_normal = normalize(world_normal.xyz);
    f_uv = v_uv;
    f_color = v_color;
}
