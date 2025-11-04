#version 450

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;
layout (location = 3) in vec3 f_color;

layout (location = 0) out vec4 final_color;

// NOTE: Scene uniforms (освещение и камера)
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

// NOTE: Model uniforms (материалы)
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

// NOTE: Spot light структура
struct SpotLight {
    vec3 position;
    float _pad0;
    vec3 direction;
    float _pad1;
    vec3 color;
    float _pad2;
    float intensity;
    float radius;
    float inner_angle_cos;
    float outer_angle_cos;
};

// NOTE: Spot lights буфер (binding = 2)
layout (binding = 2, std140) uniform SpotLightsBuffer {
    SpotLight spot_lights[16];
};

// NOTE: Вычисляет Блинна-Фонга для одного источника света
vec3 blinn_phong(vec3 N, vec3 V, vec3 L, vec3 light_color, vec3 Kd, vec3 Ks, float sh) {
    // NOTE: Half-vector между V и L
    vec3 H = normalize(V + L);

    // NOTE: Диффузная компонента: K_d * cos(theta) * I_light
    float NdotL = max(0.0, dot(N, L));
    vec3 diffuse = Kd * NdotL * light_color;

    // NOTE: Спекулярная компонента: K_s * (cos(alpha))^sh * I_light
    float NdotH = max(0.0, dot(N, H));
    float specular_intensity = pow(NdotH, sh);
    vec3 specular = Ks * specular_intensity * light_color;

    return diffuse + specular;
}

// NOTE: Вычисляет освещение от spot light источника с конусом и затуханием
vec3 spot_light_contribution(vec3 N, vec3 V, vec3 P, SpotLight light, vec3 Kd, vec3 Ks, float sh) {
    // NOTE: Вектор от фрагмента к источнику света
    vec3 L = light.position - P;
    float distance = length(L);

    // NOTE: Если фрагмент дальше радиуса, нет освещения
    if (distance > light.radius) return vec3(0.0);

    L = normalize(L);

    // NOTE: Проверяем, находится ли фрагмент в конусе
    // -L потому что L указывает НА свет, а direction указывает куда светит
    float angle_cos = dot(-L, light.direction);

    // NOTE: Если вне конуса, нет вклада
    if (angle_cos < light.outer_angle_cos) return vec3(0.0);

    // NOTE: Мягкая граница между inner и outer углом
    float spot_intensity = 1.0;
    if (angle_cos < light.inner_angle_cos) {
        // NOTE: Плавный переход между inner и outer
        float t = (angle_cos - light.outer_angle_cos) /
                  (light.inner_angle_cos - light.outer_angle_cos);
        spot_intensity = t * t;
    }

    // NOTE: Затухание по расстоянию
    float attenuation = 1.0 / (1.0 + distance * distance);

    // NOTE: Вычисляем Блинни-Фонга
    vec3 contribution = blinn_phong(N, V, L, light.color, Kd, Ks, sh);

    // NOTE: Применяем интенсивность, затухание и конусное затухание
    return contribution * light.intensity * attenuation * spot_intensity;
}

void main() {
    // NOTE: Нормализуем интерполированную нормаль
    vec3 N = normalize(f_normal);

    // NOTE: Вектор в камеру (V = view_position - fragment_position)
    vec3 V = normalize(view_position - f_position);

    // NOTE: Вектор направления солнца (уже нормализован в CPU)
    vec3 L = normalize(sun_light_direction);

    // NOTE: Минимальная освещённость везде
    vec3 ambient = ambient_light_intensity * albedo_color;

    // NOTE: Вычисляем Блинн-Фонга для направленного источника света (солнце)
    vec3 directional = blinn_phong(N, V, L, sun_light_color, albedo_color, specular_color, shininess);

    // NOTE: Суммируем вклад от всех spot lights
    vec3 spot_contribution = vec3(0.0);
    for (uint i = 0u; i < spot_lights_count; ++i) {
        spot_contribution += spot_light_contribution(N, V, f_position, spot_lights[i], albedo_color, specular_color, shininess);
    }

    // NOTE: Результат = ambient + directional + spot lights
    vec3 color = ambient + directional + spot_contribution;

    // NOTE: Финальный цвет с альфа-каналом
    final_color = vec4(color, 1.0);
}
