#version 450

layout (location = 0) in vec3 f_position;
layout (location = 1) in vec3 f_normal;
layout (location = 2) in vec2 f_uv;
layout (location = 3) in vec3 f_color;

layout (location = 0) out vec4 final_color;

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

vec3 blinn_phong(vec3 N, vec3 V, vec3 L, vec3 light_color, vec3 Kd, vec3 Ks, float sh) {
    vec3 H = normalize(V + L);
    float NdotL = max(0.0, dot(N, L));
    vec3 diffuse = Kd * NdotL * light_color;
    float NdotH = max(0.0, dot(N, H));
    float specular_intensity = pow(NdotH, sh);
    vec3 specular = Ks * specular_intensity * light_color;
    return diffuse + specular;
}

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

    vec3 N = normalize(f_normal);
    vec3 V = normalize(view_position - f_position);
    vec3 L = normalize(sun_light_direction);

    vec3 ambient = ambient_light_intensity * Kd;
    vec3 directional = blinn_phong(N, V, L, sun_light_color, Kd, Ks, shininess);

    vec3 spot_contribution = vec3(0.0);
    for (uint i = 0u; i < spot_lights_count; ++i) {
        spot_contribution += spot_light_contribution(N, V, f_position, spot_lights[i], Kd, Ks, shininess);
    }

    vec3 color = ambient + directional + spot_contribution;
    color += emissive_tex;

    final_color = vec4(color, 1.0);
}
