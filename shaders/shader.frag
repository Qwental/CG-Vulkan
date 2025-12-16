#version 450

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;
layout (location = 3) in vec3 f_color;
layout (location = 4) in vec4 f_pos_light_space;  // для теней

layout (location = 0) out vec4 final_color;

layout (binding = 0, std140) uniform SceneUniforms {
    mat4 view_projection;
    mat4 light_space_matrix;  //  для теней
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

layout (binding = 2, std140) uniform SpotLightsBuffer {
    SpotLight spot_lights[16];
};

layout (set = 1, binding = 0) uniform sampler2D albedo_texture;
layout (set = 1, binding = 1) uniform sampler2D specular_texture;
layout (set = 1, binding = 2) uniform sampler2D emissive_texture;

// Shadow map в set = 0 (глобальные данные)
layout (set = 0, binding = 3) uniform sampler2D shadow_texture;


// ФУНКЦИЯ: PCF Shadow Calculation
float calculate_pcf_shadow(vec4 light_space_pos) {
    // Perspective divide
    vec3 shadow_position = light_space_pos.xyz / light_space_pos.w;

    // Transform to [0,1] range для сэмплирования текстуры
    shadow_position.xy = shadow_position.xy * 0.5 + 0.5;

    // Текущая глубина фрагмента
    float current_depth = shadow_position.z;

    // Bias для борьбы с shadow acne
    float bias = 0.000005;
    current_depth = current_depth - bias;

    // Проверка: за пределами shadow map - нет тени
    if (shadow_position.x < 0.0 || shadow_position.x > 1.0 ||
    shadow_position.y < 0.0 || shadow_position.y > 1.0 ||
    current_depth > 1.0) {
        return 1.0;  // Полностью освещено
    }

    // PCF (Percentage Closer Filtering) - 7x7 сэмплов
    float shadow_factor = 0.0;
    vec2 texel_size = 1.0 / vec2(textureSize(shadow_texture, 0));
    int sample_count = 0;

    // Усреднение по окрестности 7x7
    for (int x = -3; x <= 3; x++) {
        for (int y = -3; y <= 3; y++) {
            float pcf_depth = texture(shadow_texture,
                                      shadow_position.xy + vec2(x, y) * texel_size).r;

            // Сравнение: если текущая глубина меньше - освещено
            if (current_depth < pcf_depth) {
                shadow_factor += 1.0;
            }
            sample_count++;
        }
    }

    // Возвращаем долю освещённости (1.0 = нет тени, 0.0 = полная тень)
    return shadow_factor / float(sample_count);
}

// ============================================
// ФУНКЦИЯ: Blinn-Phong Lighting
// ============================================
vec3 blinn_phong(vec3 N, vec3 V, vec3 L, vec3 light_color, vec3 Kd, vec3 Ks, float sh) {
    vec3 H = normalize(V + L);
    float NdotL = max(0.0, dot(N, L));
    vec3 diffuse = Kd * NdotL * light_color;
    float NdotH = max(0.0, dot(N, H));
    float specular_intensity = pow(NdotH, sh);
    vec3 specular = Ks * specular_intensity * light_color;
    return diffuse + specular;
}

// ============================================
// ФУНКЦИЯ: Spot Light Contribution
// ============================================
vec3 spot_light_contribution(vec3 N, vec3 V, vec3 P, SpotLight light, vec3 Kd, vec3 Ks, float sh) {
    vec3 L = light.position - P;
    float distance = length(L);
    if (distance > light.radius) return vec3(0.0);
    L = normalize(L);
    float angle_cos = dot(-L, light.direction);
    if (angle_cos < light.outer_angle_cos) return vec3(0.0);
    float spot_intensity = 1.0;
    if (angle_cos < light.inner_angle_cos) {
        float t = (angle_cos - light.outer_angle_cos) /
        (light.inner_angle_cos - light.outer_angle_cos);
        spot_intensity = t * t;
    }
    float attenuation = 1.0 / (1.0 + distance * distance);
    vec3 contribution = blinn_phong(N, V, L, light.color, Kd, Ks, sh);
    return contribution * light.intensity * attenuation * spot_intensity;
}

void main() {
    vec3 albedo_tex = texture(albedo_texture, f_uv).rgb;
    vec3 specular_tex = texture(specular_texture, f_uv).rgb;
    vec3 emissive_tex = texture(emissive_texture, f_uv).rgb;

    vec3 Kd = albedo_tex * albedo_color;
    vec3 Ks = specular_tex * specular_color;


    if (is_skybox > 0.5) {
        final_color = vec4(albedo_tex, 1.0);
        return;
    }

    // ============================================
    // Для обычных объектов - полный расчет освещения с тенями
    // ============================================
    vec3 N = normalize(f_normal);
    vec3 V = normalize(view_position - f_position);
    vec3 L = normalize(sun_light_direction);

    // Вычисляем shadow factor (1.0 = нет тени, 0.0 = полная тень)
    float shadow = calculate_pcf_shadow(f_pos_light_space);

    // Ambient освещение (всегда присутствует, даже в тени)
    vec3 ambient = ambient_light_intensity * Kd;

    // Directional light с учётом теней
    vec3 directional = blinn_phong(N, V, L, sun_light_color, Kd, Ks, shininess) * shadow;

    // Spot lights (без теней - можно добавить отдельно при желании)
    vec3 spot_contribution = vec3(0.0);
    for (uint i = 0u; i < spot_lights_count; ++i) {
        spot_contribution += spot_light_contribution(N, V, f_position, spot_lights[i], Kd, Ks, shininess);
    }

    // Итоговый цвет
    vec3 color = ambient + directional + spot_contribution;
    color += emissive_tex;

    final_color = vec4(color, 1.0);
}
