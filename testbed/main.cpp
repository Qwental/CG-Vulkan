#include <cstdint>
#include <climits>
#include <cstring>
#include <vector>
#include <iostream>
#include <fstream>
#include <cmath>
#include <unordered_set>

#include <veekay/veekay.hpp>

#include <vulkan/vulkan_core.h>
#include <imgui.h>
#include <lodepng.h>
#include <iomanip>  // для std::setw, std::setprecision

namespace {

float animation_time = 0.0f;
float animation_speed = 1.0f;
float trajectory_radius = 1.5f;
bool animation_paused = false;
float pause_time = 0.0f;
int animation_direction = 1;
int cylinder_model_index = 0;  // NOTE: Теперь третий (0-куб, 1-цилиндр)
    // PFN_vkCmdBeginRenderingKHR dyn_vkCmdBeginRendering = nullptr;
    // PFN_vkCmdEndRenderingKHR dyn_vkCmdEndRendering = nullptr;

// NOTE: Параметры освещения (управляемы через UI)
    // paste.txt, строка ~31
    struct LightingParams {
        // NOTE: Ambient свет (рассеянный)
        // Значения из ImGui (77/255)
        veekay::vec3 ambient_color = {0.30f, 0.30f, 0.30f};

        // NOTE: Directional свет (солнце)
        veekay::vec3 directional_direction = {-1.0f, -1.0f, -1.0f};
        // Значения из ImGui (R:216, G:74, B:74)
        veekay::vec3 directional_color = {0.85f, 0.29f, 0.29f};

        // NOTE: Spot light параметры (прожектор)
        struct SpotLightUI {
            veekay::vec3 position = {0.0f, 9.638f, 1.714f};
            veekay::vec3 direction = {-0.507f, 0.304f, 0.200f};
            // Значения из ImGui (R:0, G:83, B:255)
            veekay::vec3 color = {0.0f, 0.325f, 1.0f};
            float intensity = 5.0f;
            float radius = 13.250f;
            float inner_angle = 35.419f;
            float outer_angle = 48.548f;
        } spot_light;

    } lighting_params;

constexpr uint32_t max_models = 1024;

struct Vertex {
    veekay::vec3 position;
    veekay::vec3 normal;
    veekay::vec2 uv;
    veekay::vec3 color;
};

// NOTE: Структура для хранения одного spot light
struct SpotLight {
    veekay::vec3 position;      // Позиция прожектора
    float _pad0;

    veekay::vec3 direction;     // Направление, куда светит (нормализовано)
    float _pad1;

    veekay::vec3 color;         // Цвет света (RGB)
    float _pad2;

    float intensity;            // Интенсивность (яркость)
    float radius;               // Радиус затухания (макс расстояние)
    float inner_angle_cos;      // cos(inner_angle) для полного света
    float outer_angle_cos;      // cos(outer_angle) для мягкой границы
};

// NOTE: Максимальное количество spot lights
constexpr uint32_t max_spot_lights = 16;

// NOTE: Буфер для всех spot lights
struct SpotLightsBuffer {
    SpotLight lights[max_spot_lights];
};

struct SceneUniforms {
    veekay::mat4 view_projection;
    veekay::mat4 light_space_matrix;

    // NOTE: Позиция камеры для V вектора в Блинн-Фонге
    veekay::vec3 view_position;
    float _pad0;

    // NOTE: Интенсивность ambient света (общее освещение)
    veekay::vec3 ambient_light_intensity;
    float _pad1;

    // NOTE: Направление солнца (directional light)
    veekay::vec3 sun_light_direction;
    float _pad2;

    // NOTE: Цвет солнца (для расчёта освещения)
    veekay::vec3 sun_light_color;
    float _pad3;

    // NOTE: Количество spot lights для циклов в шейдере
    uint32_t spot_lights_count;
};

struct ModelUniforms {
    veekay::mat4 model;

    // NOTE: Диффузный цвет материала (K_d из Лекции-5)
    veekay::vec3 albedo_color;
    float _pad0;

    // NOTE: Цвет отраженного блика (K_s из Лекции-5)
    veekay::vec3 specular_color;
    float _pad1;

    // NOTE: Параметр блеска (shininess из Лекции-5)
    // Чем больше, тем острее блик (типично 16-128)
    float shininess;
    float is_skybox;
    float _pad3;
    float _pad4;
};

struct Mesh {
    veekay::graphics::Buffer* vertex_buffer;
    veekay::graphics::Buffer* index_buffer;
    uint32_t indices;
};

struct Transform {
    veekay::vec3 position = {};
    veekay::vec3 scale = {1.0f, 1.0f, 1.0f};
    veekay::vec3 rotation = {};

    // NOTE: Model matrix (translation, rotation and scaling)
    veekay::mat4 matrix() const;
};

struct Model {
    Mesh mesh;
    Transform transform;
    veekay::vec3 albedo_color;
    size_t material_id;
    bool isSkybox = false;
};

    struct Material {
        veekay::graphics::Texture* albedo;
        veekay::graphics::Texture* specular;
        veekay::graphics::Texture* emissive;
        VkDescriptorSet descriptor_set;
    };



struct Camera {
    constexpr static float default_fov = 60.0f;
    constexpr static float default_near_plane = 0.01f;
    constexpr static float default_far_plane = 100.0f;
    constexpr static float mouse_sensitivity = 0.1f;
    veekay::vec3 position = {};
    veekay::vec3 rotation = {};
    // NOTE: Pitch (вертикальный угол, градусы) угол поворота вверх/вниз
    // NOTE: Yaw (горизонтальный угол, градусы) угол поворота влево/вправо
    float pitch = 0.0f;
    float yaw = -90.0f;
    float fov = default_fov;
    float near_plane = default_near_plane;
    float far_plane = default_far_plane;

    // NOTE: View matrix of camera (inverse of a transform)
    veekay::mat4 view() const;

    // NOTE: View and projection composition
    veekay::mat4 view_projection(float aspect_ratio) const;
};

// NOTE: Scene objects
inline namespace {
    Camera camera{
        .position = {-3.5f, -3.5f, -5.0f},
        .pitch = 30.0f,
        .yaw = 0.0f
    };
    bool camera_follows_sun = false;
    std::vector<Model> models;
}

// NOTE: Vulkan objects
inline namespace {

    std::vector<Material> materials;

    size_t wood_material_id;
    size_t oak_material_id;
    size_t grass_material_id;
    size_t cobblestone_material_id;


    VkDescriptorSetLayout texture_descriptor_layout;
    VkDescriptorSet texture_descriptor_set;

    veekay::graphics::Texture* albedo_texture;
    veekay::graphics::Texture* white_texture;   // Заглушка для specular
    veekay::graphics::Texture* black_texture;   // Заглушка для emissive

    VkSampler texture_sampler;
    VkSampler skyboxsampler;



    VkShaderModule vertex_shader_module;
    VkShaderModule fragment_shader_module;

    VkDescriptorPool descriptor_pool;
    VkDescriptorSetLayout descriptor_set_layout;
    VkDescriptorSet descriptor_set;

    VkPipelineLayout pipeline_layout;
    VkPipeline pipeline;

    veekay::graphics::Buffer* scene_uniforms_buffer;
    veekay::graphics::Buffer* model_uniforms_buffer;
    veekay::graphics::Buffer* spot_lights_buffer;

    Mesh plane_mesh;
    Mesh cube_mesh;
    Mesh cylinder_mesh;

    veekay::graphics::Texture* missing_texture;
    VkSampler missing_texture_sampler;

    veekay::graphics::Texture* texture;






    // ===== SHADOW MAPPING =====
    constexpr uint32_t SHADOW_MAP_RESOLUTION = 8192;
    VkImage shadow_map_image = VK_NULL_HANDLE;
    VkDeviceMemory shadow_map_memory = VK_NULL_HANDLE;
    VkImageView shadow_map_view = VK_NULL_HANDLE;
    VkSampler shadow_sampler = VK_NULL_HANDLE;
    VkImageLayout shadow_map_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;

    VkPipelineLayout shadow_pipeline_layout = VK_NULL_HANDLE;
    VkPipeline shadow_pipeline = VK_NULL_HANDLE;

    PFN_vkCmdBeginRenderingKHR dyn_vkCmdBeginRendering = nullptr;
    PFN_vkCmdEndRenderingKHR dyn_vkCmdEndRendering = nullptr;

    veekay::mat4 global_light_space_matrix;
    VkShaderModule vertexshadermodule = VK_NULL_HANDLE;
    VkShaderModule fragmentshadermodule = VK_NULL_HANDLE;

}

// NOTE: Структура для хранения базисных векторов камеры
struct CameraBasis {
    veekay::vec3 forward;  // Направление, в которое смотрит камера
    veekay::vec3 right;    // Вектор вправо
    veekay::vec3 up;       // Вектор вверх
};

// NOTE: Helper function для кросс-произведения
inline veekay::vec3 cross(const veekay::vec3& a, const veekay::vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

// NOTE: Helper function для нормализации вектора
inline veekay::vec3 normalize(const veekay::vec3& v) {
    float len = std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
    if (len > 0.0001f) {
        return {v.x / len, v.y / len, v.z / len};
    }
    return v;
}

// NOTE: Helper function для скалярного произведения
inline float dot(const veekay::vec3& a, const veekay::vec3& b) {
    return a.x * b.x + a.y * b.y + a.z * b.z;
}

// Helper: length of vector
inline float vector_length(const veekay::vec3& v) {
    return std::sqrt(v.x * v.x + v.y * v.y + v.z * v.z);
}

// Helper: normalize vector
inline veekay::vec3 normalize_vector(const veekay::vec3& v) {
    float len = vector_length(v);
    if (len < 0.0001f) return v;
    return {v.x / len, v.y / len, v.z / len};
}

// Helper: cross product
inline veekay::vec3 cross_product(const veekay::vec3& a, const veekay::vec3& b) {
    return {
        a.y * b.z - a.z * b.y,
        a.z * b.x - a.x * b.z,
        a.x * b.y - a.y * b.x
    };
}

// Orthographic projection matrix
    // Orthographic projection matrix
    veekay::mat4 mat4_ortho(float left, float right, float bottom, float top, float zNear, float zFar) {
    veekay::mat4 result{};

    result[0][0] = 2.0f / (right - left);
    result[1][1] = 2.0f / (top - bottom);      // ← ИСПРАВЛЕНО: (top - bottom), а не (bottom - top)
    result[2][2] = -1.0f / (zFar - zNear);     // ← ИСПРАВЛЕНО: МИНУС и (zFar - zNear)

    result[3][0] = -(right + left) / (right - left);
    result[3][1] = -(top + bottom) / (top - bottom);  // ← ИСПРАВЛЕНО: (top - bottom)
    result[3][2] = -zNear / (zFar - zNear);    // ← ИСПРАВЛЕНО: МИНУС и (zFar - zNear)
    result[3][3] = 1.0f;

    return result;
}


// Look-at view matrix
veekay::mat4 mat4_lookat(const veekay::vec3& eye, const veekay::vec3& center, const veekay::vec3& up) {
    veekay::vec3 f = normalize_vector(center - eye);
    veekay::vec3 r = normalize_vector(cross_product(f, up));
    veekay::vec3 u = cross_product(r, f);

    veekay::mat4 result{};
    result[0][0] = r.x;
    result[1][0] = r.y;
    result[2][0] = r.z;
    result[0][1] = u.x;
    result[1][1] = u.y;
    result[2][1] = u.z;
    result[0][2] = -f.x;
    result[1][2] = -f.y;
    result[2][2] = -f.z;
    result[3][0] = -dot(r, eye);
    result[3][1] = -dot(u, eye);
    result[3][2] = dot(f, eye);
    result[3][3] = 1.0f;
    return result;
}

// Image layout transition helper
void transition_image_layout(VkCommandBuffer cmd, VkImage image,
    VkImageLayout old_layout, VkImageLayout new_layout,
    VkImageAspectFlags aspect_mask = VK_IMAGE_ASPECT_DEPTH_BIT) {

    VkImageMemoryBarrier barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barrier.oldLayout = old_layout;
    barrier.newLayout = new_layout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = aspect_mask;
    barrier.subresourceRange.baseMipLevel = 0;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.baseArrayLayer = 0;
    barrier.subresourceRange.layerCount = 1;

    VkPipelineStageFlags source_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkPipelineStageFlags destination_stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;

    if (old_layout == VK_IMAGE_LAYOUT_UNDEFINED) {
        barrier.srcAccessMask = 0;
        source_stage = VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT;
    } else if (old_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        barrier.srcAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        source_stage = VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    }

    if (new_layout == VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
        destination_stage = VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    } else if (new_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL) {
        barrier.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        destination_stage = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    }

    vkCmdPipelineBarrier(cmd, source_stage, destination_stage, 0,
        0, nullptr, 0, nullptr, 1, &barrier);
}



// NOTE: Вычисляет базисные векторы камеры из pitch/yaw
inline CameraBasis compute_camera_basis(float pitch_deg, float yaw_deg) {
    // NOTE: Преобразуем в радианы
    float pitch = pitch_deg * M_PI / 180.0f;
    float yaw = yaw_deg * M_PI / 180.0f;

    // NOTE: Back = направление "от камеры в мир"
    veekay::vec3 back = {
        std::cos(pitch) * std::sin(yaw),
        std::sin(pitch),
        std::cos(pitch) * std::cos(yaw)
    };
    back = normalize(back);

    // NOTE: Forward = -Back (камера смотрит в противоположную сторону)
    veekay::vec3 forward = {-back.x, -back.y, -back.z};

    // NOTE: Мировой вектор "вверх"
    veekay::vec3 world_up = {0.0f, 1.0f, 0.0f};

    // NOTE: Right = cross(Forward, WorldUp)
    veekay::vec3 right = cross(forward, world_up);
    right = normalize(right);

    // NOTE: Up = cross(Right, Forward)
    veekay::vec3 up = cross(right, forward);
    up = normalize(up);

    return {forward, right, up};
}

float toRadians(float degrees) {
    return degrees * static_cast<float>(M_PI) / 180.0f;
}





// veekay::mat4 Transform::matrix() const {
//     // TODO: Scaling and rotation
//     auto t = veekay::mat4::translation(position);
//     return t;
// }


    veekay::mat4 Transform::matrix() const {
    // 1. Translation matrix
    veekay::mat4 translation = veekay::mat4::translation(position);

    // 2. Scale matrix
    veekay::mat4 scale_mat = veekay::mat4::identity();
    scale_mat.elements[0][0] = scale.x;
    scale_mat.elements[1][1] = scale.y;
    scale_mat.elements[2][2] = scale.z;

    // 3. Rotation matrices (если используешь rotation)
    // Для базовой версии можно пропустить, если rotation = {0,0,0}

    // 4. Combine: T * R * S (порядок важен!)
    return translation * scale_mat;
}


veekay::mat4 Camera::view() const {
    // NOTE: Вычисляем базисные векторы камеры из pitch/yaw
    CameraBasis basis = compute_camera_basis(pitch, yaw);

    // NOTE: Вычисляем view матрицу через look-at матрицу
    veekay::mat4 view = veekay::mat4::identity();

    // NOTE: Заполняем матрицу по столбцам
    view.elements[0][0] = basis.right.x;
    view.elements[1][0] = basis.right.y;
    view.elements[2][0] = basis.right.z;
    view.elements[3][0] = -dot(basis.right, position);

    view.elements[0][1] = basis.up.x;
    view.elements[1][1] = basis.up.y;
    view.elements[2][1] = basis.up.z;
    view.elements[3][1] = -dot(basis.up, position);

    view.elements[0][2] = -basis.forward.x;
    view.elements[1][2] = -basis.forward.y;
    view.elements[2][2] = -basis.forward.z;
    view.elements[3][2] = dot(basis.forward, position);

    view.elements[0][3] = 0.0f;
    view.elements[1][3] = 0.0f;
    view.elements[2][3] = 0.0f;
    view.elements[3][3] = 1.0f;

    return view;
}

veekay::mat4 Camera::view_projection(float aspect_ratio) const {
    auto projection = veekay::mat4::projection(fov, aspect_ratio, near_plane, far_plane);
    return view() * projection;
}

// NOTE: Loads shader byte code from file
VkShaderModule loadShaderModule(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    size_t size = file.tellg();
    std::vector<uint32_t> buffer(size / sizeof(uint32_t));
    file.seekg(0);
    file.read(reinterpret_cast<char*>(buffer.data()), size);
    file.close();

    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = buffer.data(),
    };

    VkShaderModule result;
    if (vkCreateShaderModule(veekay::app.vk_device, &info, nullptr, &result) != VK_SUCCESS) {
        return nullptr;
    }

    return result;
}

Material createMaterial(VkCommandBuffer cmd, VkDevice device,
                        const char* albedo_path,
                        VkDescriptorPool pool,
                        VkDescriptorSetLayout layout,
                        VkSampler sampler) {
    Material mat;

    // Загружаем albedo текстуру
    std::vector<uint8_t> pixels;
    unsigned int width, height;
    unsigned int error = lodepng::decode(pixels, width, height, albedo_path);

    if (error) {
        std::cerr << "Failed to load " << albedo_path << ": "
                  << lodepng_error_text(error) << "\n";
        // Fallback текстура
        uint32_t fallback[] = {0xffff00ff, 0xff000000, 0xff000000, 0xffff00ff};
        mat.albedo = new veekay::graphics::Texture(cmd, 2, 2,
                                                    VK_FORMAT_R8G8B8A8_UNORM,
                                                    fallback);
    } else {
        mat.albedo = new veekay::graphics::Texture(cmd, width, height,
                                                    VK_FORMAT_R8G8B8A8_UNORM,
                                                    pixels.data());
    }

    // Создаём белую текстуру для specular
    veekay::vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    mat.specular = new veekay::graphics::Texture(cmd, 1, 1,
                                                  VK_FORMAT_R32G32B32A32_SFLOAT,
                                                  &white);

    // Создаём чёрную текстуру для emissive
    veekay::vec4 black = {0.0f, 0.0f, 0.0f, 1.0f};
    mat.emissive = new veekay::graphics::Texture(cmd, 1, 1,
                                                  VK_FORMAT_R32G32B32A32_SFLOAT,
                                                  &black);

    // Выделяем descriptor set для этого материала
    VkDescriptorSetAllocateInfo info{
        .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
        .descriptorPool = pool,
        .descriptorSetCount = 1,
        .pSetLayouts = &layout,
    };

    if (vkAllocateDescriptorSets(device, &info, &mat.descriptor_set) != VK_SUCCESS) {
        std::cerr << "Failed to allocate material descriptor set\n";
        return mat;
    }

    // Обновляем descriptor set
    VkDescriptorImageInfo image_infos[] = {
        {
            .sampler = sampler,
            .imageView = mat.albedo->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        {
            .sampler = sampler,
            .imageView = mat.specular->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        {
            .sampler = sampler,
            .imageView = mat.emissive->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
    };

    // ============================================
    // ИСПРАВЛЕНО: Shadow map УДАЛЁН отсюда
    // Он теперь находится в set = 0, а не в set = 1
    // ============================================

    VkWriteDescriptorSet writes[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = mat.descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[0],
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = mat.descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[1],
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = mat.descriptor_set,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[2],
        },
        // Shadow map удалён - он в set = 0 (глобальный descriptor set)
    };

    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);  // БЫЛО 4, СТАЛО 3

    return mat;
}



Mesh createBox(float x_min, float y_min, float z_min,
               float width, float height, float depth) {
    Mesh mesh;

    float x_max = x_min + width;
    float y_max = y_min + height;
    float z_max = z_min + depth;

    std::vector<Vertex> vertices = {
        // Передняя грань (Z+)
        {{x_min, y_min, z_max}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_min, z_max}, {0.0f, 0.0f, 1.0f}, {width, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_max, z_max}, {0.0f, 0.0f, 1.0f}, {width, height}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_max, z_max}, {0.0f, 0.0f, 1.0f}, {0.0f, height}, {1.0f, 1.0f, 1.0f}},

        // Задняя грань (Z-) - ИСПРАВЛЕНО
        {{x_max, y_min, z_min}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_min, z_min}, {0.0f, 0.0f, -1.0f}, {width, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_max, z_min}, {0.0f, 0.0f, -1.0f}, {width, height}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_max, z_min}, {0.0f, 0.0f, -1.0f}, {0.0f, height}, {1.0f, 1.0f, 1.0f}},

        // Верхняя грань (Y+)
        {{x_min, y_max, z_max}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_max, z_max}, {0.0f, 1.0f, 0.0f}, {width, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_max, z_min}, {0.0f, 1.0f, 0.0f}, {width, depth}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_max, z_min}, {0.0f, 1.0f, 0.0f}, {0.0f, depth}, {1.0f, 1.0f, 1.0f}},

        // Нижняя грань (Y-)
        {{x_min, y_min, z_min}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_min, z_min}, {0.0f, -1.0f, 0.0f}, {width, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_min, z_max}, {0.0f, -1.0f, 0.0f}, {width, depth}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_min, z_max}, {0.0f, -1.0f, 0.0f}, {0.0f, depth}, {1.0f, 1.0f, 1.0f}},

        // Правая грань (X+)
        {{x_max, y_min, z_max}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_min, z_min}, {1.0f, 0.0f, 0.0f}, {depth, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_max, z_min}, {1.0f, 0.0f, 0.0f}, {depth, height}, {1.0f, 1.0f, 1.0f}},
        {{x_max, y_max, z_max}, {1.0f, 0.0f, 0.0f}, {0.0f, height}, {1.0f, 1.0f, 1.0f}},

        // Левая грань (X-)
        {{x_min, y_min, z_min}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_min, z_max}, {-1.0f, 0.0f, 0.0f}, {depth, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_max, z_max}, {-1.0f, 0.0f, 0.0f}, {depth, height}, {1.0f, 1.0f, 1.0f}},
        {{x_min, y_max, z_min}, {-1.0f, 0.0f, 0.0f}, {0.0f, height}, {1.0f, 1.0f, 1.0f}},
    };

    std::vector<uint32_t> indices = {
        0, 1, 2, 2, 3, 0,       // Front
        4, 5, 6, 6, 7, 4,       // Back
        8, 9, 10, 10, 11, 8,    // Top
        12, 13, 14, 14, 15, 12, // Bottom
        16, 17, 18, 18, 19, 16, // Right
        20, 21, 22, 22, 23, 20, // Left
    };

    mesh.vertex_buffer = new veekay::graphics::Buffer(
        vertices.size() * sizeof(Vertex), vertices.data(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    mesh.index_buffer = new veekay::graphics::Buffer(
        indices.size() * sizeof(uint32_t), indices.data(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    mesh.indices = static_cast<uint32_t>(indices.size());

    return mesh;
}
   // Функция для создания дерева Minecraft на заданной позиции
void createMinecraftTree(float tree_x, float tree_y, float tree_z, size_t trunk_material_id, size_t leaves_material_id) {
    // СТВОЛ ДЕРЕВА (4 блока высотой)
    for (int i = 0; i < 4; i++) {
        Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
        models.emplace_back(Model{
            .mesh = mesh,
            .transform = Transform{.position = {tree_x, tree_y - i, tree_z}},
            .albedo_color = {1.0f, 1.0f, 1.0f},
            .material_id = trunk_material_id,  // Используем переданный ID
        });
    }

    // ЛИСТВА - Слой 1 (y = 3)
    int layer1_pattern[5][5] = {
        {0, 0, 1, 0, 0},
        {0, 1, 1, 1, 0},
        {1, 1, 1, 1, 1},
        {0, 1, 1, 1, 0},
        {0, 0, 1, 0, 0}
    };

    for (int x = 0; x < 5; x++) {
        for (int z = 0; z < 5; z++) {
            if (layer1_pattern[z][x] == 1) {
                Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
                models.emplace_back(Model{
                    .mesh = mesh,
                    .transform = Transform{.position = {tree_x + x - 2.0f, tree_y - 3.0f, tree_z + z - 2.0f}},
                    .albedo_color = {1.0f, 1.0f, 1.0f},
                    .material_id = leaves_material_id,  // Используем переданный ID
                });
            }
        }
    }

    // Слой 2 (y = 4) - полный квадрат 5x5
    for (int x = 0; x < 5; x++) {
        for (int z = 0; z < 5; z++) {
            Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
            models.emplace_back(Model{
                .mesh = mesh,
                .transform = Transform{.position = {tree_x + x - 2.0f, tree_y - 4.0f, tree_z + z - 2.0f}},
                .albedo_color = {1.0f, 1.0f, 1.0f},
                .material_id = leaves_material_id,
            });
        }
    }

    // Слой 3 (y = 5) - крестообразная форма
    int layer3_pattern[5][5] = {
        {0, 0, 1, 0, 0},
        {0, 1, 1, 1, 0},
        {1, 1, 1, 1, 1},
        {0, 1, 1, 1, 0},
        {0, 0, 1, 0, 0}
    };

    for (int x = 0; x < 5; x++) {
        for (int z = 0; z < 5; z++) {
            if (layer3_pattern[z][x] == 1) {
                Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
                models.emplace_back(Model{
                    .mesh = mesh,
                    .transform = Transform{.position = {tree_x + x - 2.0f, tree_y - 5.0f, tree_z + z - 2.0f}},
                    .albedo_color = {1.0f, 1.0f, 1.0f},
                    .material_id = leaves_material_id,
                });
            }
        }
    }

    // Слой 4 (y = 6, верхушка) - маленький крест 3x3
    int layer4_pattern[3][3] = {
        {0, 1, 0},
        {1, 1, 1},
        {0, 1, 0}
    };

    for (int x = 0; x < 3; x++) {
        for (int z = 0; z < 3; z++) {
            if (layer4_pattern[z][x] == 1) {
                Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f);
                models.emplace_back(Model{
                    .mesh = mesh,
                    .transform = Transform{.position = {tree_x + x - 1.0f, tree_y - 6.0f, tree_z + z - 1.0f}},
                    .albedo_color = {1.0f, 1.0f, 1.0f},
                    .material_id = leaves_material_id,
                });
            }
        }
    }
}


void initialize(VkCommandBuffer cmd) {
    VkDevice& device = veekay::app.vk_device;
    VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;


    // Загрузить Dynamic Rendering функции
    dyn_vkCmdBeginRendering = reinterpret_cast<PFN_vkCmdBeginRenderingKHR>(vkGetDeviceProcAddr(device, "vkCmdBeginRendering"));
    dyn_vkCmdEndRendering = reinterpret_cast<PFN_vkCmdEndRenderingKHR>(vkGetDeviceProcAddr(device, "vkCmdEndRendering"));
    if (!dyn_vkCmdBeginRendering) {
        dyn_vkCmdBeginRendering = (PFN_vkCmdBeginRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdBeginRenderingKHR");
        dyn_vkCmdEndRendering = (PFN_vkCmdEndRenderingKHR)vkGetDeviceProcAddr(device, "vkCmdEndRenderingKHR");
    }
    if (!dyn_vkCmdBeginRendering) {
        std::cerr << "CRITICAL ERROR: Could not load vkCmdBeginRendering!" << std::endl;
        veekay::app.running = false;
        return;
    }



    // NOTE: Build graphics pipeline
    {
        vertex_shader_module = loadShaderModule("./shaders/shader.vert.spv");
        if (!vertex_shader_module) {
            std::cerr << "Failed to load Vulkan vertex shader from file\n";
            veekay::app.running = false;
            return;
        }

        fragment_shader_module = loadShaderModule("./shaders/shader.frag.spv");
        if (!fragment_shader_module) {
            std::cerr << "Failed to load Vulkan fragment shader from file\n";
            veekay::app.running = false;
            return;
        }

        VkPipelineShaderStageCreateInfo stage_infos[2];

        // NOTE: Vertex shader stage
        stage_infos[0] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_shader_module,
            .pName = "main",
        };

        // NOTE: Fragment shader stage
        stage_infos[1] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_shader_module,
            .pName = "main",
        };

        // NOTE: How many bytes does a vertex take?
        VkVertexInputBindingDescription buffer_binding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };

        // NOTE: Declare vertex attributes
        VkVertexInputAttributeDescription attributes[] = {
            {
                .location = 0,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, position),
            },
            {
                .location = 1,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, normal),
            },
            {
                .location = 2,
                .binding = 0,
                .format = VK_FORMAT_R32G32_SFLOAT,
                .offset = offsetof(Vertex, uv),
            },
            {
                .location = 3,
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, color),
            },
        };

        // NOTE: Describe inputs
        VkPipelineVertexInputStateCreateInfo input_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &buffer_binding,
            .vertexAttributeDescriptionCount = std::size(attributes),
            .pVertexAttributeDescriptions = attributes,
        };

        // NOTE: Every three vertices make up a triangle
        VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };

        // NOTE: Declare clockwise triangle order as front-facing
        VkPipelineRasterizationStateCreateInfo raster_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_NONE,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1.0f,
        };

        // NOTE: Use 1 sample per pixel
        VkPipelineMultisampleStateCreateInfo sample_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO,
            .rasterizationSamples = VK_SAMPLE_COUNT_1_BIT,
            .sampleShadingEnable = false,
            .minSampleShading = 1.0f,
        };

        VkViewport viewport{
            .x = 0.0f,
            .y = 0.0f,
            .width = static_cast<float>(veekay::app.window_width),
            .height = static_cast<float>(veekay::app.window_height),
            .minDepth = 0.0f,
            .maxDepth = 1.0f,
        };

        VkRect2D scissor{
            .offset = {0, 0},
            .extent = {veekay::app.window_width, veekay::app.window_height},
        };

        // NOTE: Let rasterizer draw on the entire window
        VkPipelineViewportStateCreateInfo viewport_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };

        // NOTE: Let rasterizer perform depth-testing
        VkPipelineDepthStencilStateCreateInfo depth_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,
            .depthWriteEnable = true,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        };

        // NOTE: Let fragment shader write all the color channels
        VkPipelineColorBlendAttachmentState attachment_info{
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                            VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT,
        };

        // NOTE: Let rasterizer just copy resulting pixels onto a buffer
        VkPipelineColorBlendStateCreateInfo blend_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = false,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &attachment_info
        };

        {
            VkDescriptorPoolSize pools[] = {
                {
                    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .descriptorCount = 8,
                },
                {
                    .type = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                    .descriptorCount = 8,
                },
                {
                    .type = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 8,
                }
            };

            VkDescriptorPoolCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO,
                .maxSets = 20,
                .poolSizeCount = std::size(pools),
                .pPoolSizes = pools,
            };

            if (vkCreateDescriptorPool(device, &info, nullptr,
                                      &descriptor_pool) != VK_SUCCESS) {
                std::cerr << "Failed to create Vulkan descriptor pool\n";
                veekay::app.running = false;
                return;
            }
        }

        // NOTE: Descriptor set layout specification
        {
            VkDescriptorSetLayoutBinding bindings[] = {
                {
                    .binding = 0,
                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                },
                {
                    .binding = 1,
                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                },
                {
                    // NOTE: Spot lights буфер
                    .binding = 2,
                    .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                },
             {
                 .binding = 3,  // Shadow map
                 .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                 .descriptorCount = 1,
                 .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
             }
            };

            VkDescriptorSetLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = std::size(bindings),
                .pBindings = bindings,
            };

            if (vkCreateDescriptorSetLayout(device, &info, nullptr,
                                           &descriptor_set_layout) != VK_SUCCESS) {
                std::cerr << "Failed to create Vulkan descriptor set layout\n";
                veekay::app.running = false;
                return;
            }
        }

        {
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = descriptor_pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &descriptor_set_layout,
            };

            if (vkAllocateDescriptorSets(device, &info, &descriptor_set) != VK_SUCCESS) {
                std::cerr << "Failed to create Vulkan descriptor set\n";
                veekay::app.running = false;
                return;
            }
        }

        // ============================================
        // НОВОЕ: Создаём descriptor set layout для текстур (set = 1)
        // ============================================
        {
            VkDescriptorSetLayoutBinding texture_bindings[] = {
                {
                    .binding = 0,  // albedo_texture
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                },
                {
                    .binding = 1,  // specular_texture
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                },
                {
                    .binding = 2,  // emissive_texture
                    .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                    .descriptorCount = 1,
                    .stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT,
                },
            };

            VkDescriptorSetLayoutCreateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO,
                .bindingCount = std::size(texture_bindings),
                .pBindings = texture_bindings,
            };

            if (vkCreateDescriptorSetLayout(device, &info, nullptr,
                                            &texture_descriptor_layout) != VK_SUCCESS) {
                std::cerr << "Failed to create texture descriptor set layout\n";
                veekay::app.running = false;
                return;
                                            }
        }

        // Выделяем descriptor set для текстур
        {
            VkDescriptorSetAllocateInfo info{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO,
                .descriptorPool = descriptor_pool,
                .descriptorSetCount = 1,
                .pSetLayouts = &texture_descriptor_layout,
            };

            if (vkAllocateDescriptorSets(device, &info, &texture_descriptor_set) != VK_SUCCESS) {
                std::cerr << "Failed to allocate texture descriptor set\n";
                veekay::app.running = false;
                return;
            }
        }



        // NOTE: Declare external data sources (освещение + текстуры)
        VkDescriptorSetLayout layouts[] = {
            descriptor_set_layout,      // set = 0 (освещение)
            texture_descriptor_layout,  // set = 1 (текстуры)
        };

        VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 2,  // БЫЛО 1, СТАЛО 2
            .pSetLayouts = layouts,
        };


        // NOTE: Create pipeline layout
        if (vkCreatePipelineLayout(device, &layout_info,
                                 nullptr, &pipeline_layout) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan pipeline layout\n";
            veekay::app.running = false;
            return;
        }

        VkGraphicsPipelineCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO,
            .stageCount = 2,
            .pStages = stage_infos,
            .pVertexInputState = &input_state_info,
            .pInputAssemblyState = &assembly_state_info,
            .pViewportState = &viewport_info,
            .pRasterizationState = &raster_info,
            .pMultisampleState = &sample_info,
            .pDepthStencilState = &depth_info,
            .pColorBlendState = &blend_info,
            .layout = pipeline_layout,
            .renderPass = veekay::app.vk_render_pass,
        };

        // NOTE: Create graphics pipeline
        if (vkCreateGraphicsPipelines(device, nullptr,
                                     1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan pipeline\n";
            veekay::app.running = false;
            return;
        }


            // ===== СОЗДАНИЕ SHADOW PIPELINE =====

    // 1. Загрузить shadow vertex shader
    VkShaderModule shadow_vert_module = loadShaderModule("./shaders/shadow.vert.spv");
    if (!shadow_vert_module) {
        shadow_vert_module = vertexshadermodule;  // Fallback
    }

    VkPipelineShaderStageCreateInfo shadow_vert_stage{};
    shadow_vert_stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    shadow_vert_stage.stage = VK_SHADER_STAGE_VERTEX_BIT;
    shadow_vert_stage.module = shadow_vert_module;
    shadow_vert_stage.pName = "main";

    // 2. Push constants для shadow pipeline
    struct ShadowPushConstants {
        veekay::mat4 light_space;
        veekay::mat4 model;
    };

    VkPushConstantRange push_constant_range{};
    push_constant_range.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
    push_constant_range.offset = 0;
    push_constant_range.size = sizeof(ShadowPushConstants);

    // 3. Shadow pipeline layout
    VkPipelineLayoutCreateInfo shadow_layout_info{};
    shadow_layout_info.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    shadow_layout_info.setLayoutCount = 0;
    shadow_layout_info.pSetLayouts = nullptr;
    shadow_layout_info.pushConstantRangeCount = 1;
    shadow_layout_info.pPushConstantRanges = &push_constant_range;

    if (vkCreatePipelineLayout(device, &shadow_layout_info, nullptr, &shadow_pipeline_layout) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow pipeline layout");
    }

    // 4. Shadow vertex input (только position)
    VkVertexInputAttributeDescription shadow_attrib{};
    shadow_attrib.location = 0;
    shadow_attrib.binding = 0;
    shadow_attrib.format = VK_FORMAT_R32G32B32_SFLOAT;
    shadow_attrib.offset = offsetof(Vertex, position);

    VkPipelineVertexInputStateCreateInfo shadow_input_state{};
    shadow_input_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    shadow_input_state.vertexBindingDescriptionCount = 1;
    shadow_input_state.pVertexBindingDescriptions = &buffer_binding;
    shadow_input_state.vertexAttributeDescriptionCount = 1;
    shadow_input_state.pVertexAttributeDescriptions = &shadow_attrib;

    // 5. Shadow rasterization state
    VkPipelineRasterizationStateCreateInfo shadow_raster_info{};
    shadow_raster_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    shadow_raster_info.polygonMode = VK_POLYGON_MODE_FILL;
    shadow_raster_info.cullMode = VK_CULL_MODE_BACK_BIT;  // Избегаем shadow acne
    shadow_raster_info.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    shadow_raster_info.depthBiasEnable = VK_TRUE;
    shadow_raster_info.lineWidth = 1.0f;

    // 6. Shadow viewport state
    VkPipelineViewportStateCreateInfo shadow_viewport_state{};
    shadow_viewport_state.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    shadow_viewport_state.viewportCount = 1;
    shadow_viewport_state.scissorCount = 1;

    // 7. Dynamic state для shadow pipeline
    VkDynamicState shadow_dynamic_states[] = {
        VK_DYNAMIC_STATE_VIEWPORT,
        VK_DYNAMIC_STATE_SCISSOR,
        VK_DYNAMIC_STATE_DEPTH_BIAS
    };

    VkPipelineDynamicStateCreateInfo shadow_dynamic_state{};
    shadow_dynamic_state.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    shadow_dynamic_state.dynamicStateCount = 3;
    shadow_dynamic_state.pDynamicStates = shadow_dynamic_states;

    // 8. Pipeline Rendering Create Info (для Dynamic Rendering)
    VkPipelineRenderingCreateInfo shadow_rendering_info{};
    shadow_rendering_info.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    shadow_rendering_info.colorAttachmentCount = 0;
    shadow_rendering_info.pColorAttachmentFormats = nullptr;
    shadow_rendering_info.depthAttachmentFormat = VK_FORMAT_D32_SFLOAT;
    shadow_rendering_info.stencilAttachmentFormat = VK_FORMAT_UNDEFINED;

    // 9. Создать shadow pipeline
    VkGraphicsPipelineCreateInfo shadow_pipeline_info{};
    shadow_pipeline_info.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    shadow_pipeline_info.pNext = &shadow_rendering_info;
    shadow_pipeline_info.stageCount = 1;
    shadow_pipeline_info.pStages = &shadow_vert_stage;
    shadow_pipeline_info.pVertexInputState = &shadow_input_state;
    shadow_pipeline_info.pInputAssemblyState = &assembly_state_info;
    shadow_pipeline_info.pViewportState = &shadow_viewport_state;
    shadow_pipeline_info.pRasterizationState = &shadow_raster_info;
    shadow_pipeline_info.pMultisampleState = &sample_info;
    shadow_pipeline_info.pDepthStencilState = &depth_info;
    shadow_pipeline_info.pColorBlendState = nullptr;
    shadow_pipeline_info.pDynamicState = &shadow_dynamic_state;
    shadow_pipeline_info.layout = shadow_pipeline_layout;
    shadow_pipeline_info.renderPass = VK_NULL_HANDLE;  // Dynamic Rendering

    if (vkCreateGraphicsPipelines(device, nullptr, 1, &shadow_pipeline_info, nullptr, &shadow_pipeline) != VK_SUCCESS) {
        std::cerr << "Failed to create shadow pipeline" << std::endl;
    } else {
        std::cout << "Shadow pipeline created successfully" << std::endl;
    }

    if (shadow_vert_module != vertexshadermodule) {
        vkDestroyShaderModule(device, shadow_vert_module, nullptr);
    }

    }

    scene_uniforms_buffer = new veekay::graphics::Buffer(
        sizeof(SceneUniforms),
        nullptr,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    model_uniforms_buffer = new veekay::graphics::Buffer(
        max_models * veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms)),
        nullptr,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    // NOTE: Spot lights буфер
    spot_lights_buffer = new veekay::graphics::Buffer(
        sizeof(SpotLightsBuffer),
        nullptr,
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT);

    // NOTE: This texture and sampler is used when texture could not be loaded
    {
        VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE,
        };

        if (vkCreateSampler(device, &info, nullptr, &missing_texture_sampler) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan texture sampler\n";
            veekay::app.running = false;
            return;
        }

        uint32_t pixels[] = {
            0xff000000, 0xffff00ff,
            0xffff00ff, 0xff000000,
        };

        missing_texture = new veekay::graphics::Texture(cmd, 2, 2,
                                                        VK_FORMAT_B8G8R8A8_UNORM,
                                                        pixels);
    }


    // ============================================
    // НОВОЕ: Загрузка материалов
    // ============================================

    // Создаём сэмплер для текстур
    {
        VkSamplerCreateInfo info{
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_NEAREST,  // Minecraft style!
            .minFilter = VK_FILTER_NEAREST,
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT,
            .maxAnisotropy = 1.0f,
        };

        if (vkCreateSampler(device, &info, nullptr, &texture_sampler) != VK_SUCCESS) {
            std::cerr << "Failed to create texture sampler\n";
            veekay::app.running = false;
            return;
        }



        // ДОБАВЬТЕ НОВЫЙ СЭМПЛЕР ДЛЯ СКАЙБОКСА
        VkSampler skyboxsampler;
        VkSamplerCreateInfo skyboxinfo {
            .sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
            .magFilter = VK_FILTER_LINEAR,          // LINEAR вместо NEAREST для гладких переходов
            .minFilter = VK_FILTER_LINEAR,          // LINEAR вместо NEAREST
            .mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR,
            .addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // CLAMP вместо REPEAT
            .addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // CLAMP вместо REPEAT
            .addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE, // CLAMP вместо REPEAT
            .maxAnisotropy = 1.0f,
        };
        if (vkCreateSampler(device, &skyboxinfo, nullptr, &skyboxsampler) != VK_SUCCESS) {
            std::cerr << "Failed to create skybox sampler" << std::endl;
            veekay::app.running = false;
            return;
        }
        //
        // // ===== SHADOW MAPPING =====
        // constexpr uint32_t SHADOW_MAP_RESOLUTION = 4096;
        // VkImage shadow_map_image = VK_NULL_HANDLE;
        // VkDeviceMemory shadow_map_memory = VK_NULL_HANDLE;
        // VkImageView shadow_map_view = VK_NULL_HANDLE;
        // VkSampler shadow_sampler = VK_NULL_HANDLE;
        // VkImageLayout shadow_map_current_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        //
        // VkPipelineLayout shadow_pipeline_layout = VK_NULL_HANDLE;
        // VkPipeline shadow_pipeline = VK_NULL_HANDLE;
        //
        // PFN_vkCmdBeginRenderingKHR dyn_vkCmdBeginRendering = nullptr;
        // PFN_vkCmdEndRenderingKHR dyn_vkCmdEndRendering = nullptr;
        //
        // veekay::mat4 global_light_space_matrix;


    // ===== СОЗДАНИЕ SHADOW MAP =====

    // 1. Создать Image
    VkImageCreateInfo shadow_image_info{};
    shadow_image_info.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    shadow_image_info.imageType = VK_IMAGE_TYPE_2D;
    shadow_image_info.format = VK_FORMAT_D32_SFLOAT;
    shadow_image_info.extent = {SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION, 1};
    shadow_image_info.mipLevels = 1;
    shadow_image_info.arrayLayers = 1;
    shadow_image_info.samples = VK_SAMPLE_COUNT_1_BIT;
    shadow_image_info.tiling = VK_IMAGE_TILING_OPTIMAL;
    shadow_image_info.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    shadow_image_info.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (vkCreateImage(device, &shadow_image_info, nullptr, &shadow_map_image) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map image");
    }

    // 2. Выделить память
    VkMemoryRequirements mem_reqs;
    vkGetImageMemoryRequirements(device, shadow_map_image, &mem_reqs);

    VkMemoryAllocateInfo alloc_info{};
    alloc_info.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    alloc_info.allocationSize = mem_reqs.size;

    VkPhysicalDeviceMemoryProperties mem_props;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &mem_props);

    for (uint32_t i = 0; i < mem_props.memoryTypeCount; i++) {
        if ((mem_reqs.memoryTypeBits & (1 << i)) &&
            (mem_props.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc_info.memoryTypeIndex = i;
            break;
        }
    }

    if (vkAllocateMemory(device, &alloc_info, nullptr, &shadow_map_memory) != VK_SUCCESS) {
        throw std::runtime_error("Failed to allocate shadow map memory");
    }

    vkBindImageMemory(device, shadow_map_image, shadow_map_memory, 0);

    // 3. Создать Image View
    VkImageViewCreateInfo view_info{};
    view_info.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    view_info.image = shadow_map_image;
    view_info.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view_info.format = VK_FORMAT_D32_SFLOAT;
    view_info.subresourceRange.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    view_info.subresourceRange.levelCount = 1;
    view_info.subresourceRange.layerCount = 1;

    if (vkCreateImageView(device, &view_info, nullptr, &shadow_map_view) != VK_SUCCESS) {
        throw std::runtime_error("Failed to create shadow map image view");
    }

    // 4. Создать Shadow Sampler
    VkSamplerCreateInfo shadow_sampler_info{};
    shadow_sampler_info.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    shadow_sampler_info.magFilter = VK_FILTER_LINEAR;
    shadow_sampler_info.minFilter = VK_FILTER_LINEAR;
    shadow_sampler_info.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow_sampler_info.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow_sampler_info.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    shadow_sampler_info.compareEnable = VK_FALSE;
    shadow_sampler_info.borderColor = VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;

    if (vkCreateSampler(device, &shadow_sampler_info, nullptr, &shadow_sampler) != VK_SUCCESS) {
        std::cerr << "Failed to create shadow sampler" << std::endl;
        veekay::app.running = false;
        return;
    }

    std::cout << "Shadow map created successfully: " << SHADOW_MAP_RESOLUTION << "x" << SHADOW_MAP_RESOLUTION << std::endl;




    }

    // 1. Wood (дерево)
    materials.push_back(createMaterial(cmd, device, "assets/wood.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    wood_material_id = materials.size() - 1;
    std::cout << "Wood material ID: " << wood_material_id << std::endl;

    // 2. Oak (дуб)
    materials.push_back(createMaterial(cmd, device, "assets/дуб.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    oak_material_id = materials.size() - 1;
    std::cout << "Oak material ID: " << oak_material_id << std::endl;

    // 3. Cobblestone (булыжник)
    materials.push_back(createMaterial(cmd, device, "assets/cobblestone.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    cobblestone_material_id = materials.size() - 1;
    std::cout << "Cobblestone material ID: " << cobblestone_material_id << std::endl;

    // 4. Grass (трава) - для земли
    materials.push_back(createMaterial(cmd, device, "assets/grass.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    grass_material_id = materials.size() - 1;
    std::cout << "Grass material ID: " << grass_material_id << std::endl;

    // 5. Lenna (для цилиндра)
    materials.push_back(createMaterial(cmd, device, "assets/lenna.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    size_t lenna_material_id = materials.size() - 1;
    std::cout << "Lenna material ID: " << lenna_material_id << std::endl;

    // Загружаем skybox текстуру
    materials.push_back(createMaterial(cmd, device, "assets/skybox.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       skyboxsampler));
    size_t skybox_material_id = materials.size() - 1;
    std::cout << "Skybox material ID: " << skybox_material_id << std::endl;
    // 7. Glowstone (светящийся камень)
    materials.push_back(createMaterial(cmd, device, "assets/Glowstone.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    size_t glowstone_material_id = materials.size() - 1;
    std::cout << "Glowstone material ID: " << glowstone_material_id << std::endl;

    // 8. ЛИСТВА
    materials.push_back(createMaterial(cmd, device, "assets/листва.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    size_t leaves_material_id = materials.size() - 1;
    std::cout << "leaves_material_id material ID: " << leaves_material_id << std::endl;

    // 9. Water (вода)
    materials.push_back(createMaterial(cmd, device, "assets/water.png",
                                       descriptor_pool, texture_descriptor_layout,
                                       texture_sampler));
    size_t water_material_id = materials.size() - 1;
    std::cout << "Water material ID: " << water_material_id << std::endl;


// Загружаем albedo текстуру из PNG
{
    std::vector<uint8_t> pixels;
    uint32_t width, height;

    unsigned int error = lodepng::decode(pixels, width, height, "assets/grass.png");

    if (error) {
        std::cerr << "lodepng error: " << lodepng_error_text(error) << "\n";
        std::cerr << "Using fallback texture\n";
        // Создаём заглушку если не удалось загрузить
        uint32_t fallback_pixels[] = {
            0xffff00ff, 0xff000000,
            0xff000000, 0xffff00ff,
        };
        albedo_texture = new veekay::graphics::Texture(cmd, 2, 2,
                                                       VK_FORMAT_R8G8B8A8_UNORM,
                                                       fallback_pixels);
    } else {
        albedo_texture = new veekay::graphics::Texture(cmd, width, height,
                                                       VK_FORMAT_R8G8B8A8_UNORM,
                                                       pixels.data());
    }
}

// Создаём белую текстуру (заглушка для specular)
{
    veekay::vec4 white = {1.0f, 1.0f, 1.0f, 1.0f};
    white_texture = new veekay::graphics::Texture(cmd, 1, 1,
                                                   VK_FORMAT_R32G32B32A32_SFLOAT,
                                                   &white);
}

// Создаём чёрную текстуру (заглушка для emissive)
{
    veekay::vec4 black = {0.0f, 0.0f, 0.0f, 1.0f};
    black_texture = new veekay::graphics::Texture(cmd, 1, 1,
                                                   VK_FORMAT_R32G32B32A32_SFLOAT,
                                                   &black);
}

// Обновляем descriptor set для текстур
{
    VkDescriptorImageInfo image_infos[] = {
        {
            .sampler = texture_sampler,
            .imageView = albedo_texture->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        {
            .sampler = texture_sampler,
            .imageView = white_texture->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
        {
            .sampler = texture_sampler,
            .imageView = black_texture->view,
            .imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
        },
    };

    VkWriteDescriptorSet writes[] = {
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture_descriptor_set,
            .dstBinding = 0,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[0],
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture_descriptor_set,
            .dstBinding = 1,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[1],
        },
        {
            .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
            .dstSet = texture_descriptor_set,
            .dstBinding = 2,
            .descriptorCount = 1,
            .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
            .pImageInfo = &image_infos[2],
        },
    };

    vkUpdateDescriptorSets(device, 3, writes, 0, nullptr);
}


    {
        VkDescriptorBufferInfo buffer_infos[] = {
            {
                .buffer = scene_uniforms_buffer->buffer,
                .offset = 0,
                .range = sizeof(SceneUniforms),
            },
            {
                .buffer = model_uniforms_buffer->buffer,
                .offset = 0,
                .range = sizeof(ModelUniforms),
            },
            {
                // NOTE: Spot lights буфер
                .buffer = spot_lights_buffer->buffer,
                .offset = 0,
                .range = sizeof(SpotLightsBuffer),
            },
        };
        VkDescriptorImageInfo shadow_image_info_desc{};
        shadow_image_info_desc.sampler = shadow_sampler;
        shadow_image_info_desc.imageView = shadow_map_view;
        shadow_image_info_desc.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkWriteDescriptorSet write_infos[] = {
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptor_set,
                .dstBinding = 0,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &buffer_infos[0],
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptor_set,
                .dstBinding = 1,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER_DYNAMIC,
                .pBufferInfo = &buffer_infos[1],
            },
            {
                // NOTE: Spot lights буфер
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptor_set,
                .dstBinding = 2,
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,
                .pBufferInfo = &buffer_infos[2],
            },
            {
                .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET,
                .dstSet = descriptor_set,
                .dstBinding = 3, // Shadow Map Binding
                .dstArrayElement = 0,
                .descriptorCount = 1,
                .descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER,
                .pImageInfo = &shadow_image_info_desc,
            }
        };

        vkUpdateDescriptorSets(device, std::size(write_infos), write_infos, 0, nullptr);

    }

    // NOTE: Plane mesh initialization
    {
        std::vector<Vertex> vertices = {
            {{-5.0f, -2.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            {{5.0f, -2.0f, 5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
            {{5.0f, -2.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
            {{-5.0f, -2.0f, -5.0f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        };

        std::vector<uint32_t> indices = {
            0, 1, 2, 2, 3, 0
        };

        plane_mesh.vertex_buffer = new veekay::graphics::Buffer(
            vertices.size() * sizeof(Vertex), vertices.data(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

        plane_mesh.index_buffer = new veekay::graphics::Buffer(
            indices.size() * sizeof(uint32_t), indices.data(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        plane_mesh.indices = static_cast<uint32_t>(indices.size());
    }

    // NOTE: Cube mesh initialization
    {
        std::vector<Vertex> vertices = {
            // NOTE: Front face (Z+)
            {{-0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
            {{0.5f, -0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 0.0f, 0.0f}},
            {{0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},
            {{-0.5f, 0.5f, 0.5f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 0.0f, 0.0f}},

            // NOTE: Back face (Z-)
            {{0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
            {{-0.5f, -0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {0.0f, 1.0f, 0.0f}},
            {{-0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},
            {{0.5f, 0.5f, -0.5f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {0.0f, 1.0f, 0.0f}},

            // NOTE: Top face (Y+)
            {{-0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
            {{0.5f, 0.5f, 0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 0.0f}, {0.0f, 0.0f, 1.0f}},
            {{0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {1.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},
            {{-0.5f, 0.5f, -0.5f}, {0.0f, 1.0f, 0.0f}, {0.0f, 1.0f}, {0.0f, 0.0f, 1.0f}},

            // NOTE: Bottom face (Y-)
            {{-0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 0.0f}},
            {{0.5f, -0.5f, -0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 0.0f}},
            {{0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 0.0f}},
            {{-0.5f, -0.5f, 0.5f}, {0.0f, -1.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 0.0f}},

            // NOTE: Right face (X+)
            {{0.5f, -0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 0.5f, 0.0f}},
            {{0.5f, -0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 0.5f, 0.0f}},
            {{0.5f, 0.5f, -0.5f}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 0.5f, 0.0f}},
            {{0.5f, 0.5f, 0.5f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 0.5f, 0.0f}},

            // NOTE: Left face (X-)
            {{-0.5f, -0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {0.5f, 0.0f, 1.0f}},
            {{-0.5f, -0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {0.5f, 0.0f, 1.0f}},
            {{-0.5f, 0.5f, 0.5f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {0.5f, 0.0f, 1.0f}},
            {{-0.5f, 0.5f, -0.5f}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {0.5f, 0.0f, 1.0f}},
        };

        std::vector<uint32_t> indices = {
            // Front
            0, 1, 2, 2, 3, 0,
            // Back
            4, 5, 6, 6, 7, 4,
            // Top
            8, 9, 10, 10, 11, 8,
            // Bottom
            12, 13, 14, 14, 15, 12,
            // Right
            16, 17, 18, 18, 19, 16,
            // Left
            20, 21, 22, 22, 23, 20,
        };

        cube_mesh.vertex_buffer = new veekay::graphics::Buffer(
            vertices.size() * sizeof(Vertex), vertices.data(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

        cube_mesh.index_buffer = new veekay::graphics::Buffer(
            indices.size() * sizeof(uint32_t), indices.data(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        cube_mesh.indices = static_cast<uint32_t>(indices.size());
    }

    // NOTE: Cylinder mesh initialization
    {
        const int segments = 16;
        const float height = 2.0f;
        const float radius = 0.5f;

        std::vector<Vertex> vertices;
        std::vector<uint32_t> indices;

        veekay::vec3 white_color = {1.0f, 1.0f, 1.0f};
        veekay::vec3 blue_color = {0.0f, 0.4f, 1.0f};

        // NOTE: Создаем вершины боковой поверхности цилиндра
        for (int i = 0; i < segments; ++i) {
            float angle1 = 2.0f * M_PI * i / segments;
            float angle2 = 2.0f * M_PI * (i + 1) / segments;

            float x1 = cosf(angle1) * radius;
            float z1 = sinf(angle1) * radius;
            float x2 = cosf(angle2) * radius;
            float z2 = sinf(angle2) * radius;

            // NOTE: Нормаль направлена радиально наружу
            veekay::vec3 normal1 = {cosf(angle1), 0.0f, sinf(angle1)};
            veekay::vec3 normal2 = {cosf(angle2), 0.0f, sinf(angle2)};

            // NOTE: Первый треугольник
            vertices.push_back({
                {x1, -height/2.0f, z1},
                normal1,
                {static_cast<float>(i) / segments, 0.0f},
                blue_color
            });
            vertices.push_back({
                {x1, height/2.0f, z1},
                normal1,
                {static_cast<float>(i) / segments, 1.0f},
                white_color
            });
            vertices.push_back({
                {x2, -height/2.0f, z2},
                normal2,
                {static_cast<float>(i + 1) / segments, 0.0f},
                blue_color
            });

            // NOTE: Второй треугольник
            vertices.push_back({
                {x1, height/2.0f, z1},
                normal1,
                {static_cast<float>(i) / segments, 1.0f},
                white_color
            });
            vertices.push_back({
                {x2, height/2.0f, z2},
                normal2,
                {static_cast<float>(i + 1) / segments, 1.0f},
                white_color
            });
            vertices.push_back({
                {x2, -height/2.0f, z2},
                normal2,
                {static_cast<float>(i + 1) / segments, 0.0f},
                blue_color
            });
        }

        // NOTE: Все индексы просто по порядку
        for (uint32_t i = 0; i < vertices.size(); ++i) {
            indices.push_back(i);
        }

        cylinder_mesh.vertex_buffer = new veekay::graphics::Buffer(
            vertices.size() * sizeof(Vertex), vertices.data(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

        cylinder_mesh.index_buffer = new veekay::graphics::Buffer(
            indices.size() * sizeof(uint32_t), indices.data(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        cylinder_mesh.indices = static_cast<uint32_t>(indices.size());
    }


    // NOTE: Wall mesh (стена 4x3)
Mesh wall_mesh;
{
    float wall_width = 4.0f;
    float wall_height = 3.0f;

    std::vector<Vertex> vertices = {
        // Front face
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{wall_width, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{wall_width, wall_height, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, wall_height, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},

        // Back face
        {{wall_width, 0.0f, -0.1f}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, 0.0f, -0.1f}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, wall_height, -0.1f}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        {{wall_width, wall_height, -0.1f}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
    };

    std::vector<uint32_t> indices = {
        0, 1, 2, 2, 3, 0,  // Front
        4, 5, 6, 6, 7, 4,  // Back
    };

    wall_mesh.vertex_buffer = new veekay::graphics::Buffer(
        vertices.size() * sizeof(Vertex), vertices.data(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    wall_mesh.index_buffer = new veekay::graphics::Buffer(
        indices.size() * sizeof(uint32_t), indices.data(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    wall_mesh.indices = static_cast<uint32_t>(indices.size());
}

    // NOTE: Ground plane mesh - большой квадрат травы ниже пола
    Mesh ground_mesh;
    {
        float ground_size = 75.0f;  // Размер поляны (50x50)
        float ground_height = 1.0f; // Высота блока
        float ground_y = -2.0f;     // Ниже пола (floor на y=-2.0f)

        std::vector<Vertex> vertices = {
            // Top face (то, что видно - трава)
            {-ground_size/2, ground_y, ground_size/2, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {ground_size/2, ground_y, ground_size/2, 0.0f, 1.0f, 0.0f, 10.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {ground_size/2, ground_y, -ground_size/2, 0.0f, 1.0f, 0.0f, 10.0f, 10.0f, 1.0f, 1.0f, 1.0f},
            {-ground_size/2, ground_y, -ground_size/2, 0.0f, 1.0f, 0.0f, 0.0f, 10.0f, 1.0f, 1.0f, 1.0f},

            // Bottom face (низ блока)
            {-ground_size/2, ground_y - ground_height, -ground_size/2, 0.0f, -1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {ground_size/2, ground_y - ground_height, -ground_size/2, 0.0f, -1.0f, 0.0f, 10.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {ground_size/2, ground_y - ground_height, ground_size/2, 0.0f, -1.0f, 0.0f, 10.0f, 10.0f, 1.0f, 1.0f, 1.0f},
            {-ground_size/2, ground_y - ground_height, ground_size/2, 0.0f, -1.0f, 0.0f, 0.0f, 10.0f, 1.0f, 1.0f, 1.0f},
        };

        std::vector<uint32_t> indices = {
            0, 1, 2, 2, 3, 0,  // Top (трава)
            4, 5, 6, 6, 7, 4,  // Bottom
        };

        ground_mesh.vertex_buffer = new veekay::graphics::Buffer(
            vertices.size() * sizeof(Vertex), vertices.data(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

        ground_mesh.index_buffer = new veekay::graphics::Buffer(
            indices.size() * sizeof(uint32_t), indices.data(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        ground_mesh.indices = static_cast<uint32_t>(indices.size());
    }


// NOTE: Skybox mesh initialization - очень большой куб
// NOTE: Skybox с правильными UV координатами для 4x3 layout
Mesh skybox_mesh;
{
    float size = 100.0f;

    // UV раскладка для текстуры 3600x2700 (4 колонки x 3 ряда)
    // Каждая грань: ширина = 1/4 = 0.25, высота = 1/3 = 0.333
std::vector<Vertex> vertices = {
    // Front face - колонка 2 (0.25 - 0.50)
    {-size, -size,  size, 0.0f, 0.0f,  1.0f, 0.25f, 0.666f, 1.0f, 1.0f, 1.0f},
    { size, -size,  size, 0.0f, 0.0f,  1.0f, 0.50f, 0.666f, 1.0f, 1.0f, 1.0f},
    { size,  size,  size, 0.0f, 0.0f,  1.0f, 0.50f, 0.333f, 1.0f, 1.0f, 1.0f},
    {-size,  size,  size, 0.0f, 0.0f,  1.0f, 0.25f, 0.333f, 1.0f, 1.0f, 1.0f},

    // Back face - колонка 4 (0.75 - 1.0)
    { size, -size, -size, 0.0f, 0.0f, -1.0f, 0.75f, 0.666f, 1.0f, 1.0f, 1.0f},
    {-size, -size, -size, 0.0f, 0.0f, -1.0f, 1.00f, 0.666f, 1.0f, 1.0f, 1.0f},
    {-size,  size, -size, 0.0f, 0.0f, -1.0f, 1.00f, 0.333f, 1.0f, 1.0f, 1.0f},
    { size,  size, -size, 0.0f, 0.0f, -1.0f, 0.75f, 0.333f, 1.0f, 1.0f, 1.0f},

    // Top face - средняя верхняя (0.0 - 0.333)
    {-size,  size,  size, 0.0f,  1.0f, 0.0f, 0.25f, 0.333f, 1.0f, 1.0f, 1.0f},
    { size,  size,  size, 0.0f,  1.0f, 0.0f, 0.50f, 0.333f, 1.0f, 1.0f, 1.0f},
    { size,  size, -size, 0.0f,  1.0f, 0.0f, 0.50f, 0.0f,   1.0f, 1.0f, 1.0f},
    {-size,  size, -size, 0.0f,  1.0f, 0.0f, 0.25f, 0.0f,   1.0f, 1.0f, 1.0f},

    // Bottom face - средняя нижняя (0.666 - 1.0)
    {-size, -size, -size, 0.0f, -1.0f, 0.0f, 0.25f, 1.0f,   1.0f, 1.0f, 1.0f},
    { size, -size, -size, 0.0f, -1.0f, 0.0f, 0.50f, 1.0f,   1.0f, 1.0f, 1.0f},
    { size, -size,  size, 0.0f, -1.0f, 0.0f, 0.50f, 0.666f, 1.0f, 1.0f, 1.0f},
    {-size, -size,  size, 0.0f, -1.0f, 0.0f, 0.25f, 0.666f, 1.0f, 1.0f, 1.0f},

    // Right face - колонка 3 (0.50 - 0.75) - ИСПРАВЛЕНО!
    { size, -size,  size,  1.0f, 0.0f, 0.0f, 0.50f, 0.666f, 1.0f, 1.0f, 1.0f},
    { size, -size, -size,  1.0f, 0.0f, 0.0f, 0.75f, 0.666f, 1.0f, 1.0f, 1.0f},
    { size,  size, -size,  1.0f, 0.0f, 0.0f, 0.75f, 0.333f, 1.0f, 1.0f, 1.0f},
    { size,  size,  size,  1.0f, 0.0f, 0.0f, 0.50f, 0.333f, 1.0f, 1.0f, 1.0f},

    // Left face - колонка 1 (0.0 - 0.25) - ИСПРАВЛЕНО!
    {-size, -size, -size, -1.0f, 0.0f, 0.0f, 0.0f,  0.666f, 1.0f, 1.0f, 1.0f},
    {-size, -size,  size, -1.0f, 0.0f, 0.0f, 0.25f, 0.666f, 1.0f, 1.0f, 1.0f},
    {-size,  size,  size, -1.0f, 0.0f, 0.0f, 0.25f, 0.333f, 1.0f, 1.0f, 1.0f},
    {-size,  size, -size, -1.0f, 0.0f, 0.0f, 0.0f,  0.333f, 1.0f, 1.0f, 1.0f},
};



    std::vector<uint32_t> indices = {
        0, 1, 2, 2, 3, 0,
        4, 5, 6, 6, 7, 4,
        8, 9, 10, 10, 11, 8,
        12, 13, 14, 14, 15, 12,
        16, 17, 18, 18, 19, 16,
        20, 21, 22, 22, 23, 20,
    };

    skybox_mesh.vertex_buffer = new veekay::graphics::Buffer(
        vertices.size() * sizeof(Vertex), vertices.data(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    skybox_mesh.index_buffer = new veekay::graphics::Buffer(
        indices.size() * sizeof(uint32_t), indices.data(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
    skybox_mesh.indices = static_cast<uint32_t>(indices.size());
}




    // NOTE: Portal screen mesh - тонкая плоскость с водой (2x3 блока)
    // NOTE: Portal screen mesh - тонкая плоскость с водой (2x3 блока)
    Mesh portal_screen_mesh;
    {
        float screen_width = 2.0f;   // 2 блока в ширину
        float screen_height = 3.0f;  // 3 блока в высоту
        float screen_thickness = 0.1f; // Тонкий параллелепипед

        std::vector<Vertex> vertices = {
            // Front face (лицевая сторона портала - видна спереди)
            {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {screen_width, 0.0f, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {screen_width, screen_height, 0.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.5f, 1.0f, 1.0f, 1.0f},
            {0.0f, screen_height, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.5f, 1.0f, 1.0f, 1.0f},

            // Back face (задняя сторона - видна сзади)
            {screen_width, 0.0f, -screen_thickness, 0.0f, 0.0f, -1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {0.0f, 0.0f, -screen_thickness, 0.0f, 0.0f, -1.0f, 1.0f, 0.0f, 1.0f, 1.0f, 1.0f},
            {0.0f, screen_height, -screen_thickness, 0.0f, 0.0f, -1.0f, 1.0f, 1.5f, 1.0f, 1.0f, 1.0f},
            {screen_width, screen_height, -screen_thickness, 0.0f, 0.0f, -1.0f, 0.0f, 1.5f, 1.0f, 1.0f, 1.0f},
        };

        std::vector<uint32_t> indices = {
            0, 1, 2, 2, 3, 0,  // Front
            4, 5, 6, 6, 7, 4,  // Back
        };

        portal_screen_mesh.vertex_buffer = new veekay::graphics::Buffer(
            vertices.size() * sizeof(Vertex), vertices.data(),
            VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

        portal_screen_mesh.index_buffer = new veekay::graphics::Buffer(
            indices.size() * sizeof(uint32_t), indices.data(),
            VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

        portal_screen_mesh.indices = static_cast<uint32_t>(indices.size());
    }



// NOTE: Pillar mesh (столб 0.3x3x0.3)
Mesh pillar_mesh;
{
    float width = 0.3f;
    float height = 3.0f;

    std::vector<Vertex> vertices = {
        // Передняя грань
        {{0.0f, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{width, 0.0f, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{width, height, 0.0f}, {0.0f, 0.0f, 1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, height, 0.0f}, {0.0f, 0.0f, 1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},

        // Правая грань
        {{width, 0.0f, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{width, 0.0f, -width}, {1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{width, height, -width}, {1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        {{width, height, 0.0f}, {1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},

        // Задняя грань
        {{width, 0.0f, -width}, {0.0f, 0.0f, -1.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, 0.0f, -width}, {0.0f, 0.0f, -1.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, height, -width}, {0.0f, 0.0f, -1.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        {{width, height, -width}, {0.0f, 0.0f, -1.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},

        // Левая грань
        {{0.0f, 0.0f, -width}, {-1.0f, 0.0f, 0.0f}, {0.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, 0.0f, 0.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 0.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, height, 0.0f}, {-1.0f, 0.0f, 0.0f}, {1.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
        {{0.0f, height, -width}, {-1.0f, 0.0f, 0.0f}, {0.0f, 1.0f}, {1.0f, 1.0f, 1.0f}},
    };

    std::vector<uint32_t> indices = {
        0, 1, 2, 2, 3, 0,    // Front
        4, 5, 6, 6, 7, 4,    // Right
        8, 9, 10, 10, 11, 8, // Back
        12, 13, 14, 14, 15, 12, // Left
    };

    pillar_mesh.vertex_buffer = new veekay::graphics::Buffer(
        vertices.size() * sizeof(Vertex), vertices.data(),
        VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);

    pillar_mesh.index_buffer = new veekay::graphics::Buffer(
        indices.size() * sizeof(uint32_t), indices.data(),
        VK_BUFFER_USAGE_INDEX_BUFFER_BIT);

    pillar_mesh.indices = static_cast<uint32_t>(indices.size());
}


    // NOTE: Add models to scene
    // models.emplace_back(Model{
    //     .mesh = plane_mesh,
    //     .transform = Transform{.position = {0.0f, 3.0f, 0.0f}},
    //     .albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f}
    // });

    // // NOTE: КУБА
    // models.emplace_back(Model{
    //     .mesh = cube_mesh,
    //     .transform = Transform{
    //         .position = {2.0f, -0.5f, 0.0f},
    //         .scale = {0.8f, 0.8f, 0.8f},
    //     },
    //     .albedo_color = veekay::vec3{0.9f, 0.2f, 0.2f}
    // });

    // NOTE: ЦИЛИНДР
    // models.emplace_back(Model{
    //     .mesh = cylinder_mesh,
    //     .transform = Transform{
    //         .position = {30.0f, -0.0f, 0.0f},
    //     },
    //     .albedo_color = veekay::vec3{0.8f, 0.8f, 0.8f}
    // });
// ============================================
// СОЗДАНИЕ МОДЕЛЕЙ
// ============================================

models.clear();


    models.emplace_back(Model{
        .mesh = cylinder_mesh,
        .transform = Transform{
            .position = {-30.0f, 1.0f, 33.0f},  // Слева от дома
            .rotation = {0.0f, 0.0f, 0.0f}
        },
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = 2
    });

    // ============================================
    // SKYBOX (рендерится первым, индекс 0)
    // ============================================
    models.emplace_back(Model{
        .mesh = skybox_mesh,
        .transform = Transform{
            .position = {0.0f, 0.0f, 0.0f},  // Центр мира
            .scale = {1.0f, 1.0f, 1.0f}
        },
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = skybox_material_id,
        .isSkybox = true
    });





    // Позиция портала в мире (поднят на 1 блок выше)
    float portal_x = -8.0f;
    float portal_y = 0.0f;  // ИЗМЕНЕНО: было 1.0f, теперь 0.0f (на 1 блок выше в Vulkan координатах)
    float portal_z = 0.0f;

    // 1. Нижняя перекладина (y=0, 4 блока в ширину) - СВЕТОКАМЕНЬ
    {
        Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 4.0f, 1.0f, 1.0f);
        models.emplace_back(Model{
            .mesh = mesh,
            .transform = Transform{.position = {portal_x, portal_y, portal_z}},
            .albedo_color = {1.0f, 1.0f, 1.0f},
            .material_id = glowstone_material_id,
        });
    }

    // 2. Верхняя перекладина (y=4, 4 блока в ширину) - СВЕТОКАМЕНЬ
    {
        Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 4.0f, 1.0f, 1.0f);
        models.emplace_back(Model{
            .mesh = mesh,
            .transform = Transform{.position = {portal_x, portal_y + 4.0f, portal_z}},
            .albedo_color = {1.0f, 1.0f, 1.0f},
            .material_id = glowstone_material_id,
        });
    }

    // 3. Левая колонна (x=0, y=1-3, 3 блока в высоту) - СВЕТОКАМЕНЬ
    {
        Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 1.0f);
        models.emplace_back(Model{
            .mesh = mesh,
            .transform = Transform{.position = {portal_x, portal_y + 1.0f, portal_z}},
            .albedo_color = {1.0f, 1.0f, 1.0f},
            .material_id = glowstone_material_id,
        });
    }

    // 4. Правая колонна (x=3, y=1-3, 3 блока в высоту) - СВЕТОКАМЕНЬ
    {
        Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 1.0f);
        models.emplace_back(Model{
            .mesh = mesh,
            .transform = Transform{.position = {portal_x + 3.0f, portal_y + 1.0f, portal_z}},
            .albedo_color = {1.0f, 1.0f, 1.0f},
            .material_id = glowstone_material_id,
        });
    }

    // 5. Экран портала (2x3 блока, в центре, с водой)
    models.emplace_back(Model{
        .mesh = portal_screen_mesh,
        .transform = Transform{.position = {portal_x + 1.0f, portal_y + 1.0f, portal_z + 0.4f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = water_material_id,
    });

    int cylindermodelindex = 1;
  // ============================================
// MINECRAFT ДОМ
// ============================================
    models.emplace_back(Model{
        .mesh = ground_mesh,
        .transform = Transform{.position = {0.0f, +8.0f, 0.0f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = grass_material_id,  // Используем текстуру травы
    });
// потолок
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 5.0f, 1.0f, 5.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {0.0f, 0.0f, 0.0f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id =  oak_material_id
    });
}





// СТЕНЫ (wood)

// Левая стена (x=0) - сдвигаем по Z на +1
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 3.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {0.0f, 1.0f, 1.0f}}, // Z: 0→1
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = wood_material_id
    });
}

// Правая стена (x=4) - сдвигаем по Z на +1
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 3.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {4.0f, 1.0f, 1.0f}}, // Z: 0→1
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = wood_material_id
    });
}

// Задняя стена (z=0) - сдвигаем по X на +1
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 3.0f, 3.0f, 1.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {1.0f, 1.0f, 0.0f}}, // X: 0→1
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = wood_material_id
    });
}

// Передняя стена (z=4) - сдвигаем по X на +1
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 3.0f, 3.0f, 1.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {1.0f, 1.0f, 4.0f}}, // X: 0→1
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = wood_material_id
    });
}

// КОЛОННЫ (oak) - оставляем как есть

// Левая передняя (0,1,0)
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 1.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {0.0f, 1.0f, 0.0f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = oak_material_id
    });
}

// Правая передняя (4,1,0)
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 1.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {4.0f, 1.0f, 0.0f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = oak_material_id
    });
}

// Левая задняя (0,1,4)
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 1.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {0.0f, 1.0f, 4.0f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = oak_material_id
    });
}

// Правая задняя (4,1,4)
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 1.0f, 3.0f, 1.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {4.0f, 1.0f, 4.0f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = oak_material_id
    });
}

// пол (oak)
{
    Mesh mesh = createBox(0.0f, 0.0f, 0.0f, 5.0f, 1.0f, 5.0f);
    models.emplace_back(Model{
        .mesh = mesh,
        .transform = Transform{.position = {0.0f, 4.0f, 0.0f}},
        .albedo_color = {1.0f, 1.0f, 1.0f},
        .material_id = cobblestone_material_id
    });
}

    // Параметры надписи
    float text_start_x = -18.0f;  // Начальная позиция X
    float text_start_y = -8.0f;    // Y-координата (на уровне земли)
    float text_start_z = 10.0f;  // Z-координата (сдвиг назад, чтобы не пересекаться)
    float block_size = 1.0f;      // Размер одного блока

    // Паттерн надписи MINECRAFT (5 строк × 37 колонок)
    // 1 = cobblestone, 0 = пустота
    int pattern[5][37] = {
        {1,0,0,0,1,0,1,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1,0,1,1,1},
        {1,1,0,1,1,0,1,0,1,1,0,0,1,0,1,0,0,0,1,0,0,0,1,0,1,0,1,0,1,0,1,0,0,0,0,1,0},
        {1,0,1,0,1,0,1,0,1,0,1,0,1,0,1,1,0,0,1,0,0,0,1,1,0,0,1,1,1,0,1,1,0,0,0,1,0},
        {1,0,0,0,1,0,1,0,1,0,0,1,1,0,1,0,0,0,1,0,0,0,1,0,1,0,1,0,1,0,1,0,0,0,0,1,0},
        {1,0,0,0,1,0,1,0,1,0,0,0,1,0,1,1,1,0,1,1,1,0,1,0,1,0,1,0,1,0,1,0,0,0,0,1,0}
    };

    // Создаем блоки cobblestone по паттерну
    for (int row = 0; row < 5; row++) {
        for (int col = 0; col < 37; col++) {
            if (pattern[row][col] == 1) {
                // Создаем блок 1x1x1
                Mesh mesh = createBox(0.0f, 0.0f, 0.0f, block_size, block_size, block_size);

                // Вычисляем позицию блока
                float x = text_start_x + col * block_size;
                float y = text_start_y + row * block_size;
                float z = text_start_z;

                models.emplace_back(Model{
                    .mesh = mesh,
                    .transform = Transform{.position = {x, y, z}},
                    .albedo_color = {1.0f, 1.0f, 1.0f},
                    .material_id = cobblestone_material_id,
                });
            }
        }
    }

    // Создаём деревья с передачей material ID
    createMinecraftTree(10.0f, 5.0f, -5.0f, oak_material_id, leaves_material_id);
    createMinecraftTree(15.0f, 5.0f, 0.0f, oak_material_id, leaves_material_id);
    createMinecraftTree(-10.0f, 5.0f, -10.0f, oak_material_id, leaves_material_id);


    // ===== 8 НОВЫХ ДЕРЕВЬЕВ =====
    // Группа справа (положительный X)
    createMinecraftTree(18.0f, 5.0f, -8.0f, oak_material_id, leaves_material_id);   // Дерево 4 - дальний правый угол
    createMinecraftTree(20.0f, 5.0f, 5.0f, oak_material_id, leaves_material_id);    // Дерево 5 - правый передний
    createMinecraftTree(12.0f, 5.0f, 8.0f, oak_material_id, leaves_material_id);    // Дерево 6 - ближе к центру справа

    // Группа слева (отрицательный X)
    createMinecraftTree(-15.0f, 5.0f, -5.0f, oak_material_id, leaves_material_id);  // Дерево 7 - левый средний
    createMinecraftTree(-18.0f, 5.0f, 5.0f, oak_material_id, leaves_material_id);   // Дерево 8 - левый передний
    createMinecraftTree(-12.0f, 5.0f, -15.0f, oak_material_id, leaves_material_id); // Дерево 9 - левый задний

    // Группа сзади (отрицательный Z)
    createMinecraftTree(5.0f, 5.0f, -15.0f, oak_material_id, leaves_material_id);   // Дерево 10 - задний центр
    createMinecraftTree(-5.0f, 5.0f, -18.0f, oak_material_id, leaves_material_id);  // Дерево 11 - дальний задний

    // Дополнительные деревья по краям
    createMinecraftTree(8.0f, 5.0f, 12.0f, oak_material_id, leaves_material_id);    // Дерево 12 - передний правый
}

// NOTE: Destroy resources here
void shutdown() {
    VkDevice& device = veekay::app.vk_device;
    // Shadow resources
    if (shadow_sampler != VK_NULL_HANDLE) {
        vkDestroySampler(device, shadow_sampler, nullptr);
    }
    if (shadow_map_view != VK_NULL_HANDLE) {
        vkDestroyImageView(device, shadow_map_view, nullptr);
    }
    if (shadow_map_image != VK_NULL_HANDLE) {
        vkDestroyImage(device, shadow_map_image, nullptr);
    }
    if (shadow_map_memory != VK_NULL_HANDLE) {
        vkFreeMemory(device, shadow_map_memory, nullptr);
    }
    if (shadow_pipeline != VK_NULL_HANDLE) {
        vkDestroyPipeline(device, shadow_pipeline, nullptr);
    }
    if (shadow_pipeline_layout != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(device, shadow_pipeline_layout, nullptr);
    }
    // NOTE: Очистка материалов
    for (auto& mat : materials) {
        delete mat.emissive;
        delete mat.specular;
        delete mat.albedo;
    }
    materials.clear();

    // NOTE: Очистка сэмплеров и текстур
    vkDestroySampler(device, texture_sampler, nullptr);
    vkDestroySampler(device, skyboxsampler, nullptr);
    delete black_texture;
    delete white_texture;
    delete albedo_texture;

    vkDestroyDescriptorSetLayout(device, texture_descriptor_layout, nullptr);

    vkDestroySampler(device, missing_texture_sampler, nullptr);
    delete missing_texture;

    // NOTE: Очистка базовых мешей
    delete cylinder_mesh.index_buffer;
    delete cylinder_mesh.vertex_buffer;

    delete cube_mesh.index_buffer;
    delete cube_mesh.vertex_buffer;

    delete plane_mesh.index_buffer;
    delete plane_mesh.vertex_buffer;

    // ============================================
    // НОВОЕ: Очистка динамических мешей из models
    // ============================================
    // Используем unordered_set чтобы не удалять один и тот же буфер дважды
    std::unordered_set<veekay::graphics::Buffer*> uniqueBuffers;

    for (const auto& model : models) {
        // Пропускаем базовые меши, которые мы уже удаляем выше
        if (model.mesh.vertex_buffer != cylinder_mesh.vertex_buffer &&
            model.mesh.vertex_buffer != cube_mesh.vertex_buffer &&
            model.mesh.vertex_buffer != plane_mesh.vertex_buffer) {
            uniqueBuffers.insert(model.mesh.vertex_buffer);
            uniqueBuffers.insert(model.mesh.index_buffer);
        }
    }

    // NOTE: Удаляем уникальные буферы
    for (auto* buffer : uniqueBuffers) {
        delete buffer;
    }

    models.clear();

    // NOTE: Очистка буферов uniform
    delete model_uniforms_buffer;
    delete scene_uniforms_buffer;
    delete spot_lights_buffer;

    // NOTE: Очистка дескрипторов
    vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

    // NOTE: Очистка пайплайна и шейдеров
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyShaderModule(device, fragment_shader_module, nullptr);
    vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {
    ImGui::Begin("Controls & Lighting:");
    ImGui::SetWindowSize(ImVec2(400, 500), ImGuiCond_FirstUseEver);

    // NOTE: КАТЕГОРИЯ: Анимация
    ImGui::SeparatorText("Animation");
    ImGui::SliderFloat("Trajectory Radius##traj", &trajectory_radius, 0.1f, 3.0f);
    ImGui::SliderFloat("Animation Speed##speed", &animation_speed, 0.1f, 5.0f);
    ImGui::Checkbox("Pause##pause", &animation_paused);
    if (ImGui::Button("Reverse Direction##reverse")) {
        animation_direction *= -1;
    }

    ImGui::Checkbox("Animation paused", &animation_paused);
    ImGui::Checkbox("Follow sun", &camera_follows_sun);

    if (ImGui::Button("EXIT")) {
        veekay::app.running = false;
    }

    // NOTE: КАТЕГОРИЯ: Ambient свет
    ImGui::SeparatorText("Ambient Light");
    ImGui::ColorEdit3("Ambient Color##amb", (float*)&lighting_params.ambient_color);

    // NOTE: КАТЕГОРИЯ: Directional свет (солнце)
    ImGui::SeparatorText("Directional Light (Sun)");
    ImGui::SliderFloat3("Sun Direction##dir", (float*)&lighting_params.directional_direction, -1.0f, 1.0f);
    ImGui::ColorEdit3("Sun Color##sun", (float*)&lighting_params.directional_color);

    // NOTE: КАТЕГОРИЯ: Spot Light
    ImGui::SeparatorText("Spot Light (Projector)");
    ImGui::SliderFloat3("Spot Position##spos", (float*)&lighting_params.spot_light.position, -10.0f, 10.0f);
    ImGui::SliderFloat3("Spot Direction##sdir", (float*)&lighting_params.spot_light.direction, -1.0f, 1.0f);
    ImGui::ColorEdit3("Spot Color##scol", (float*)&lighting_params.spot_light.color);
    ImGui::SliderFloat("Spot Intensity##sint", &lighting_params.spot_light.intensity, 0.0f, 5.0f);
    ImGui::SliderFloat("Spot Radius##srad", &lighting_params.spot_light.radius, 1.0f, 50.0f);
    ImGui::SliderFloat("Inner Angle (deg)##inner", &lighting_params.spot_light.inner_angle, 1.0f, 89.0f);
    ImGui::SliderFloat("Outer Angle (deg)##outer", &lighting_params.spot_light.outer_angle, 1.0f, 89.0f);

    ImGui::End();

    // NOTE: Вычисляем delta_time для анимации
    static double last_time = 0.0;
    double delta_time = time - last_time;
    last_time = time;

    // NOTE: Обновляем время анимации
    if (!animation_paused) {
        animation_time += (float)delta_time * animation_speed * animation_direction;
    } else {
        pause_time = animation_time;
    }

    // NOTE: Параметрические уравнения восьмёрки
    float t = animation_paused ? pause_time : animation_time;
    float denominator = 1.0f + sin(t) * sin(t);
    float x = trajectory_radius * cos(t) / denominator;
    float z = trajectory_radius * sin(t) * cos(t) / denominator;

    // ============================================
    // НОВОЕ: Применяем позицию к ЦИЛИНДРУ (индекс 0)
    // ============================================
    if (models.size() > cylinder_model_index) {
        models[cylinder_model_index].transform.position = {x + 20.0f, 4.0f, z+ 20.0f};

        // NOTE: Self-rotation цилиндра вокруг оси Y
        models[cylinder_model_index].transform.rotation.y += 0.5f;  // Градусов за кадр
    }

    // NOTE: Обработка камеры и управления
    if (!ImGui::IsWindowHovered()) {
        using namespace veekay::input;

        if (mouse::isButtonDown(mouse::Button::left)) {
            auto move_delta = mouse::cursorDelta();

            camera.yaw -= move_delta.x * camera.mouse_sensitivity;
            camera.pitch -= move_delta.y * camera.mouse_sensitivity;

            if (camera.pitch > 89.0f)  camera.pitch = 89.0f;
            if (camera.pitch < -89.0f) camera.pitch = -89.0f;

            CameraBasis basis = compute_camera_basis(camera.pitch, camera.yaw);


            if (camera_follows_sun) {
                veekay::vec3 sun_direction = normalize(lighting_params.directional_direction);
                veekay::vec3 center = {0.0f, 0.0f, 0.0f};
veekay::vec3 sun_position = sun_direction * 30.0f;
                camera.position = sun_position;
                veekay::vec3 to_center = center - camera.position;
                to_center = normalize(to_center);
                camera.pitch = -asinf(to_center.y) * 180.0f / M_PI;
                camera.yaw = atan2f(to_center.z, to_center.x) * 180.0f / M_PI;
            }









            veekay::vec3 forward = basis.forward;
            veekay::vec3 right = basis.right;
            veekay::vec3 up = {0.0f, 1.0f, 0.0f};

            constexpr float camera_speed = 0.1f;

            if (keyboard::isKeyDown(keyboard::Key::w))
                camera.position -= forward * camera_speed;

            if (keyboard::isKeyDown(keyboard::Key::s))
                camera.position += forward * camera_speed;

            if (keyboard::isKeyDown(keyboard::Key::d))
                camera.position += right * camera_speed;

            if (keyboard::isKeyDown(keyboard::Key::a))
                camera.position -= right * camera_speed;

            if (keyboard::isKeyDown(keyboard::Key::q))
                camera.position += up * camera_speed;

            if (keyboard::isKeyDown(keyboard::Key::z))
                camera.position -= up * camera_speed;

        }
    }

    float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);

 static bool first_frame = true;

    veekay::vec3 scene_min = {  9999.0f,  9999.0f,  9999.0f };
    veekay::vec3 scene_max = { -9999.0f, -9999.0f, -9999.0f };

    for (const Model& m : models) {
        if (m.isSkybox) continue;

        scene_min.x = std::min(scene_min.x, m.transform.position.x);
        scene_min.y = std::min(scene_min.y, m.transform.position.y);
        scene_min.z = std::min(scene_min.z, m.transform.position.z);

        scene_max.x = std::max(scene_max.x, m.transform.position.x);
        scene_max.y = std::max(scene_max.y, m.transform.position.y);
        scene_max.z = std::max(scene_max.z, m.transform.position.z);
    }

    veekay::vec3 scene_center = {
        (scene_min.x + scene_max.x) * 0.5f,
        (scene_min.y + scene_max.y) * 0.5f,
        (scene_min.z + scene_max.z) * 0.5f
    };

    float scene_radius = std::max({
        scene_max.x - scene_min.x,
        scene_max.y - scene_min.y,
        scene_max.z - scene_min.z
    }) * 0.5f * 1.2f;

    if (first_frame) {
        std::cout << "\n=== SCENE SETUP (first frame) ===" << std::endl;
        std::cout << "Min: (" << scene_min.x << ", " << scene_min.y << ", " << scene_min.z << ")" << std::endl;
        std::cout << "Max: (" << scene_max.x << ", " << scene_max.y << ", " << scene_max.z << ")" << std::endl;
        std::cout << "Center: (" << scene_center.x << ", " << scene_center.y << ", " << scene_center.z << ")" << std::endl;
        std::cout << "Radius: " << scene_radius << std::endl;
        first_frame = false;
    }

    // Вычисление light matrix
    veekay::vec3 light_dir = normalize_vector(lighting_params.directional_direction);
    float light_distance = scene_radius * 2.5f;
    veekay::vec3 light_pos = scene_center + light_dir * light_distance;

    veekay::mat4 light_view = mat4_lookat(light_pos, scene_center, {0.0f, 1.0f, 0.0f});

    float frustum_extent = scene_radius * 1.5f;
    veekay::mat4 light_projection = mat4_ortho(
        -frustum_extent,  frustum_extent,
        -frustum_extent,  frustum_extent,
         0.1f,            light_distance * 2.5f
    );

    global_light_space_matrix = light_view * light_projection;

    SceneUniforms scene_uniforms{
        .view_projection    = camera.view_projection(aspect_ratio),
        .light_space_matrix = global_light_space_matrix,
        .view_position      = camera.position,
        .ambient_light_intensity = lighting_params.ambient_color,
        .sun_light_direction     = normalize(lighting_params.directional_direction),
        .sun_light_color         = lighting_params.directional_color,
        .spot_lights_count       = 0,
    };

    // NOTE: Копируем в GPU буфер
    *(SceneUniforms*)scene_uniforms_buffer->mapped_region = scene_uniforms;

    // ============================================
    // НОВОЕ: Находим индекс кубика с прожектором
    // ============================================
    size_t cube_index = models.size() - 1;  // Последний объект (кубик)

    if (models.size() > cube_index) {
        veekay::vec3 cube_position = models[cube_index].transform.position;

        // NOTE: Создаём и заполняем spot lights
        SpotLightsBuffer spot_lights_data{};

        float inner_angle = lighting_params.spot_light.inner_angle * M_PI / 180.0f;
        float outer_angle = lighting_params.spot_light.outer_angle * M_PI / 180.0f;

        // NOTE: Прожектор привязан к кубику (на верхушке)
        spot_lights_data.lights[0] = SpotLight{
            .position = cube_position + veekay::vec3{0.0f, 0.5f, 0.0f},
            .direction = normalize(lighting_params.spot_light.direction),
            .color = lighting_params.spot_light.color,
            .intensity = lighting_params.spot_light.intensity,
            .radius = lighting_params.spot_light.radius,
            .inner_angle_cos = std::cos(inner_angle),
            .outer_angle_cos = std::cos(outer_angle),
        };

        // NOTE: Остальные отключены
        for (uint32_t i = 1; i < max_spot_lights; ++i) {
            spot_lights_data.lights[i] = SpotLight{
                .position = {0.0f, 0.0f, 0.0f},
                .direction = {0.0f, -1.0f, 0.0f},
                .color = {0.0f, 0.0f, 0.0f},
                .intensity = 0.0f,
                .radius = 1.0f,
                .inner_angle_cos = 1.0f,
                .outer_angle_cos = 0.0f,
            };
        }

        // NOTE: Копируем spot lights в GPU буфер
        *(SpotLightsBuffer*)spot_lights_buffer->mapped_region = spot_lights_data;
    }

    // ============================================
    // НОВОЕ: Заполняем ModelUniforms с учётом материалов
    // ============================================
    std::vector<ModelUniforms> model_uniforms(models.size());
    for (size_t i = 0, n = models.size(); i < n; ++i) {
        const Model& model = models[i];
        ModelUniforms& uniforms = model_uniforms[i];

        // NOTE: Матрица трансформации объекта
        uniforms.model = model.transform.matrix();

        // NOTE: Диффузный цвет (альбедо)
        uniforms.albedo_color = model.albedo_color;
        uniforms.is_skybox = model.isSkybox ? 1.0f : 0.0f;


        // ============================================
        // НОВОЕ: Устанавливаем материалы в зависимости от ID
        // ============================================
        if (model.material_id == cobblestone_material_id) {
            // Булыжник: матовый
            uniforms.specular_color = {0.2f, 0.2f, 0.2f};
            uniforms.shininess = 8.0f;
        }
        else if (model.material_id == wood_material_id) {
            // Дерево: средний блеск
            uniforms.specular_color = {0.4f, 0.4f, 0.4f};
            uniforms.shininess = 32.0f;
        }
        else if (model.material_id == oak_material_id) {
            // Дуб: глянцевый
            uniforms.specular_color = {0.6f, 0.6f, 0.6f};
            uniforms.shininess = 64.0f;
        }
        else if (i == cylinder_model_index) {
            // Цилиндр (Lenna): блестящий
            uniforms.specular_color = {1.0f, 1.0f, 1.0f};
            uniforms.shininess = 128.0f;
        }
        else {
            // По умолчанию
            uniforms.specular_color = {0.5f, 0.5f, 0.5f};
            uniforms.shininess = 32.0f;
        }
    }

    // NOTE: Копируем ModelUniforms в GPU буфер с выравниванием
    const size_t alignment = veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));

    for (size_t i = 0, n = model_uniforms.size(); i < n; ++i) {
        const ModelUniforms& uniforms = model_uniforms[i];
        char* const pointer = static_cast<char*>(model_uniforms_buffer->mapped_region) + i * alignment;
        *reinterpret_cast<ModelUniforms*>(pointer) = uniforms;
    }
}

// ============================================
// ФУНКЦИЯ: Рендеринг теней (Shadow Pass)
// ============================================
// void render_shadow_pass(VkCommandBuffer cmd) {
//     // ============================================
//     // ЛОГГИРОВАНИЕ: Начало shadow pass
//     // ============================================
//     std::cout << "\n========== SHADOW PASS START ==========" << std::endl;
//     std::cout << "Total models: " << models.size() << std::endl;
//
//     // 1. Transition: UNDEFINED → DEPTH_ATTACHMENT
//     transition_image_layout(cmd, shadow_map_image,
//         shadow_map_current_layout,
//         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
//     shadow_map_current_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
//     std::cout << "Shadow map transitioned to DEPTH_ATTACHMENT_OPTIMAL" << std::endl;
//
//     // 2. Begin Dynamic Rendering
//     VkRenderingAttachmentInfo depth_attachment{};
//     depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
//     depth_attachment.imageView = shadow_map_view;
//     depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
//     depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
//     depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
//     depth_attachment.clearValue.depthStencil = {1.0f, 0};
//
//     VkRenderingInfo rendering_info{};
//     rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
//     rendering_info.renderArea = {{0, 0}, {SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION}};
//     rendering_info.layerCount = 1;
//     rendering_info.colorAttachmentCount = 0;
//     rendering_info.pColorAttachments = nullptr;
//     rendering_info.pDepthAttachment = &depth_attachment;
//
//     dyn_vkCmdBeginRendering(cmd, &rendering_info);
//     std::cout << "Dynamic rendering started (resolution: " << SHADOW_MAP_RESOLUTION << "x" << SHADOW_MAP_RESOLUTION << ")" << std::endl;
//
//     // 3. Bind shadow pipeline
//     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);
//     std::cout << "Shadow pipeline bound" << std::endl;
//
//     // 4. Set viewport and scissor
//     VkViewport shadow_viewport{};
//     shadow_viewport.x = 0.0f;
//     shadow_viewport.y = 0.0f;
//     shadow_viewport.width = (float)SHADOW_MAP_RESOLUTION;
//     shadow_viewport.height = (float)SHADOW_MAP_RESOLUTION;
//     shadow_viewport.minDepth = 0.0f;
//     shadow_viewport.maxDepth = 1.0f;
//     vkCmdSetViewport(cmd, 0, 1, &shadow_viewport);
//
//     VkRect2D shadow_scissor{};
//     shadow_scissor.offset = {0, 0};
//     shadow_scissor.extent = {SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION};
//     vkCmdSetScissor(cmd, 0, 1, &shadow_scissor);
//     std::cout << "Viewport & Scissor set" << std::endl;
//
//     // 5. Set depth bias
//     // vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f);
//     // std::cout << "Depth bias set" << std::endl;
//     vkCmdSetDepthBias(cmd, 0.0f, 0.0f, 0.0f);  // полностью отключить bias
//
//     // ============================================
//     // 6. Рисуем все модели
//     // ============================================
//     struct ShadowPushConstants {
//         veekay::mat4 light_space;
//         veekay::mat4 model;
//     };
//
//     std::cout << "\n--- Light Space Matrix ---" << std::endl;
//     for (int row = 0; row < 4; row++) {
//         std::cout << "[";
//         for (int col = 0; col < 4; col++) {
//             // ИСПРАВЛЕНО: elements[col][row] вместо [row][col]
//             // Потому что mat4 хранится column-major (столбцы)
//             std::cout << std::setw(8) << std::fixed << std::setprecision(3)
//                       << global_light_space_matrix.elements[col][row];
//             if (col < 3) std::cout << ", ";
//         }
//         std::cout << "]" << std::endl;
//     }
//     std::cout << std::endl;
//
//
//     VkDeviceSize zero_offset = 0;
//     size_t drawn_models = 0;
//
//     for (size_t i = 0; i < models.size(); i++) {
//         const Model& model = models[i];
//
//         if (model.isSkybox) {
//             std::cout << "Model " << i << ": SKIPPED (skybox)" << std::endl;
//             continue;
//         }
//
//         ShadowPushConstants constants{
//             .light_space = global_light_space_matrix,
//             .model = model.transform.matrix()
//         };
//
//         vkCmdPushConstants(cmd, shadow_pipeline_layout,
//             VK_SHADER_STAGE_VERTEX_BIT, 0,
//             sizeof(ShadowPushConstants), &constants);
//
//         vkCmdBindVertexBuffers(cmd, 0, 1, &model.mesh.vertex_buffer->buffer, &zero_offset);
//         vkCmdBindIndexBuffer(cmd, model.mesh.index_buffer->buffer, 0, VK_INDEX_TYPE_UINT32);
//         vkCmdDrawIndexed(cmd, model.mesh.indices, 1, 0, 0, 0);
//
//         drawn_models++;
//
//         // Логгируем каждую модель
//         std::cout << "Model " << i
//                   << ": pos(" << model.transform.position.x << ", "
//                               << model.transform.position.y << ", "
//                               << model.transform.position.z << ")"
//                   << " indices=" << model.mesh.indices
//                   << " material_id=" << model.material_id
//                   << std::endl;
//     }
//
//     std::cout << "\nTotal drawn models: " << drawn_models << "/" << models.size() << std::endl;
//
//     // 7. End rendering
//     dyn_vkCmdEndRendering(cmd);
//     std::cout << "Dynamic rendering ended" << std::endl;
//
//     // 8. Transition: DEPTH_ATTACHMENT → SHADER_READ_ONLY
//     transition_image_layout(cmd, shadow_map_image,
//         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
//         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
//     shadow_map_current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
//     std::cout << "Shadow map transitioned to SHADER_READ_ONLY_OPTIMAL" << std::endl;
//
//     std::cout << "========== SHADOW PASS END ==========\n" << std::endl;
// }
//
//
// void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
//
//
//     // vkResetCommandBuffer(cmd, 0);
//
//     // {
//     //     VkCommandBufferBeginInfo info{
//     //         .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
//     //         .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
//     //     };
//     //     vkBeginCommandBuffer(cmd, &info);
//     // }
//
//     {
//         VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
//         VkClearValue clear_depth{.depthStencil = {1.0f, 0}};
//         VkClearValue clear_values[] = {clear_color, clear_depth};
//
//         VkRenderPassBeginInfo info{
//             .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
//             .renderPass = veekay::app.vk_render_pass,
//             .framebuffer = framebuffer,
//             .renderArea = {
//                 .extent = {
//                     veekay::app.window_width,
//                     veekay::app.window_height
//                 },
//             },
//             .clearValueCount = 2,
//             .pClearValues = clear_values,
//         };
//         vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
//     }
//
//     vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
//
//     VkDeviceSize zero_offset = 0;
//     VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
//     VkBuffer current_index_buffer = VK_NULL_HANDLE;
//     const size_t model_uniforms_alignment =
//         veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));
//
//     // ПЕРВЫЙ ПРОХОД: рендерим все объекты КРОМЕ скайбокса
//     for (size_t i = 0, n = models.size(); i < n; ++i) {
//         const Model& model = models[i];
//
//         // Пропускаем скайбокс
//         if (model.isSkybox) continue;
//
//         const Mesh& mesh = model.mesh;
//
//         if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
//             current_vertex_buffer = mesh.vertex_buffer->buffer;
//             vkCmdBindVertexBuffers(cmd, 0, 1, &current_vertex_buffer, &zero_offset);
//         }
//
//         if (current_index_buffer != mesh.index_buffer->buffer) {
//             current_index_buffer = mesh.index_buffer->buffer;
//             vkCmdBindIndexBuffer(cmd, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
//         }
//
//         uint32_t offset = i * model_uniforms_alignment;
//         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
//                                 0, 1, &descriptor_set, 1, &offset);
//
//         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
//                                 1, 1, &materials[model.material_id].descriptor_set, 0, nullptr);
//
//         vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
//     }
//
//     // ВТОРОЙ ПРОХОД: рендерим скайбокс с depth test = LESS_OR_EQUAL и без depth write
//     for (size_t i = 0, n = models.size(); i < n; ++i) {
//         const Model& model = models[i];
//
//         // Рендерим ТОЛЬКО скайбокс
//         if (!model.isSkybox) continue;
//
//         const Mesh& mesh = model.mesh;
//
//         if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
//             current_vertex_buffer = mesh.vertex_buffer->buffer;
//             vkCmdBindVertexBuffers(cmd, 0, 1, &current_vertex_buffer, &zero_offset);
//         }
//
//         if (current_index_buffer != mesh.index_buffer->buffer) {
//             current_index_buffer = mesh.index_buffer->buffer;
//             vkCmdBindIndexBuffer(cmd, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
//         }
//
//         uint32_t offset = i * model_uniforms_alignment;
//         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
//                                 0, 1, &descriptor_set, 1, &offset);
//
//         vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
//                                 1, 1, &materials[model.material_id].descriptor_set, 0, nullptr);
//
//         vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
//     }
//
//     vkCmdEndRenderPass(cmd);
//     // vkEndCommandBuffer(cmd);
// }




// ============================================
// ФУНКЦИЯ: Рендеринг теней (Shadow Pass)
// ============================================
void render_shadow_pass(VkCommandBuffer cmd) {
    static int call_count = 0;
    const bool should_log = (call_count < 100);
    call_count++;

    if (should_log) {
        std::cout << "\n========== SHADOW PASS START (call #" << call_count << ") ==========" << std::endl;
        std::cout << "Total models: " << models.size() << std::endl;
    }

    // 1. Transition: UNDEFINED → DEPTH_ATTACHMENT
    transition_image_layout(cmd, shadow_map_image,
        shadow_map_current_layout,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL);
    shadow_map_current_layout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;

    // 2. Begin Dynamic Rendering
    VkRenderingAttachmentInfo depth_attachment{};
    depth_attachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depth_attachment.imageView = shadow_map_view;
    depth_attachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL;
    depth_attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    depth_attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    depth_attachment.clearValue.depthStencil = {1.0f, 0};

    VkRenderingInfo rendering_info{};
    rendering_info.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering_info.renderArea = {{0, 0}, {SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION}};
    rendering_info.layerCount = 1;
    rendering_info.colorAttachmentCount = 0;
    rendering_info.pColorAttachments = nullptr;
    rendering_info.pDepthAttachment = &depth_attachment;

    dyn_vkCmdBeginRendering(cmd, &rendering_info);

    // 3. Bind shadow pipeline
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, shadow_pipeline);

    // 4. Set viewport and scissor
    VkViewport shadow_viewport{};
    shadow_viewport.x = 0.0f;
    shadow_viewport.y = 0.0f;
    shadow_viewport.width = (float)SHADOW_MAP_RESOLUTION;
    shadow_viewport.height = (float)SHADOW_MAP_RESOLUTION;
    shadow_viewport.minDepth = 0.0f;
    shadow_viewport.maxDepth = 1.0f;
    vkCmdSetViewport(cmd, 0, 1, &shadow_viewport);

    VkRect2D shadow_scissor{};
    shadow_scissor.offset = {0, 0};
    shadow_scissor.extent = {SHADOW_MAP_RESOLUTION, SHADOW_MAP_RESOLUTION};
    vkCmdSetScissor(cmd, 0, 1, &shadow_scissor);

    vkCmdSetDepthBias(cmd, 1.25f, 0.0f, 1.75f);

    // ============================================
    // 6. Рисуем все модели
    // ============================================
    struct ShadowPushConstants {
        veekay::mat4 light_space;
        veekay::mat4 model;
    };

    VkDeviceSize zero_offset = 0;
    size_t drawn_models = 0;
    size_t skipped_skybox = 0;

    for (size_t i = 0; i < models.size(); i++) {
        const Model& model = models[i];

        if (model.isSkybox) {
            skipped_skybox++;
            if (should_log && i < 5) {
                std::cout << "Model " << i << ": SKIPPED (skybox)" << std::endl;
            }
            continue;
        }

        ShadowPushConstants constants{
            .light_space = global_light_space_matrix,
            .model = model.transform.matrix()
        };

        vkCmdPushConstants(cmd, shadow_pipeline_layout,
            VK_SHADER_STAGE_VERTEX_BIT, 0,
            sizeof(ShadowPushConstants), &constants);

        vkCmdBindVertexBuffers(cmd, 0, 1, &model.mesh.vertex_buffer->buffer, &zero_offset);
        vkCmdBindIndexBuffer(cmd, model.mesh.index_buffer->buffer, 0, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cmd, model.mesh.indices, 1, 0, 0, 0);

        drawn_models++;

        // Логгируем только первые 10 моделей
        if (should_log && i < 10) {
            std::cout << "Model " << i
                      << ": pos(" << std::fixed << std::setprecision(2)
                      << model.transform.position.x << ", "
                      << model.transform.position.y << ", "
                      << model.transform.position.z << ")"
                      << " indices=" << model.mesh.indices
                      << " material_id=" << model.material_id
                      << std::endl;
        }
    }

    if (should_log) {
        std::cout << "\nSummary: drawn=" << drawn_models
                  << ", skipped_skybox=" << skipped_skybox
                  << ", total=" << models.size() << std::endl;
    }

    // 7. End rendering
    dyn_vkCmdEndRendering(cmd);

    // 8. Transition: DEPTH_ATTACHMENT → SHADER_READ_ONLY
    transition_image_layout(cmd, shadow_map_image,
        VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    shadow_map_current_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    if (should_log) {
        std::cout << "========== SHADOW PASS END ==========\n" << std::endl;
    }
}
void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    // ============================================
    // ЗАПОЛНЕНИЕ UNIFORMS
    // ============================================
    const float aspect_ratio =
        static_cast<float>(veekay::app.window_width) /
        static_cast<float>(veekay::app.window_height);

    SceneUniforms scene_uniforms{
        .view_projection         = camera.view_projection(aspect_ratio),
        .light_space_matrix      = global_light_space_matrix,
        .view_position           = camera.position,
        .ambient_light_intensity = lighting_params.ambient_color,
        .sun_light_direction     = normalize(lighting_params.directional_direction),
        .sun_light_color         = lighting_params.directional_color,
        .spot_lights_count       = 0,
    };

    std::memcpy(scene_uniforms_buffer->mapped_region, &scene_uniforms, sizeof(SceneUniforms));

    const size_t model_uniforms_alignment =
        veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));

    for (size_t i = 0; i < models.size(); ++i) {
        const Model& model = models[i];
        ModelUniforms uniforms{
            .model = model.transform.matrix(),
            .albedo_color = model.albedo_color,
            .specular_color = {0.5f, 0.5f, 0.5f},
            .shininess = 32.0f,
            .is_skybox = model.isSkybox ? 1.0f : 0.0f,  // ✅ ИСПРАВЛЕНО
        };

        size_t offset = i * model_uniforms_alignment;
        std::memcpy(
            static_cast<char*>(model_uniforms_buffer->mapped_region) + offset,
            &uniforms, sizeof(ModelUniforms)
        );
    }

    // ============================================
    // НАЧАЛО RENDER PASS
    // ============================================
    VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
    VkClearValue clear_depth{.depthStencil = {1.0f, 0}};
    VkClearValue clear_values[] = {clear_color, clear_depth};

    VkRenderPassBeginInfo info{
        .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
        .renderPass = veekay::app.vk_render_pass,
        .framebuffer = framebuffer,
        .renderArea = {
            .extent = {veekay::app.window_width, veekay::app.window_height},
        },
        .clearValueCount = 2,
        .pClearValues = clear_values,
    };
    vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkDeviceSize zero_offset = 0;
    VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
    VkBuffer current_index_buffer = VK_NULL_HANDLE;

    // ПЕРВЫЙ ПРОХОД: обычные объекты
    for (size_t i = 0, n = models.size(); i < n; ++i) {
        const Model& model = models[i];
        if (model.isSkybox) continue;

        const Mesh& mesh = model.mesh;

        if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
            current_vertex_buffer = mesh.vertex_buffer->buffer;
            vkCmdBindVertexBuffers(cmd, 0, 1, &current_vertex_buffer, &zero_offset);
        }

        if (current_index_buffer != mesh.index_buffer->buffer) {
            current_index_buffer = mesh.index_buffer->buffer;
            vkCmdBindIndexBuffer(cmd, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
        }

        uint32_t offset = i * model_uniforms_alignment;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, 1, &descriptor_set, 1, &offset);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                1, 1, &materials[model.material_id].descriptor_set, 0, nullptr);

        vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
    }

    // ВТОРОЙ ПРОХОД: скайбокс
    for (size_t i = 0, n = models.size(); i < n; ++i) {
        const Model& model = models[i];
        if (!model.isSkybox) continue;

        const Mesh& mesh = model.mesh;

        if (current_vertex_buffer != mesh.vertex_buffer->buffer) {
            current_vertex_buffer = mesh.vertex_buffer->buffer;
            vkCmdBindVertexBuffers(cmd, 0, 1, &current_vertex_buffer, &zero_offset);
        }

        if (current_index_buffer != mesh.index_buffer->buffer) {
            current_index_buffer = mesh.index_buffer->buffer;
            vkCmdBindIndexBuffer(cmd, current_index_buffer, zero_offset, VK_INDEX_TYPE_UINT32);
        }

        uint32_t offset = i * model_uniforms_alignment;
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                0, 1, &descriptor_set, 1, &offset);

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout,
                                1, 1, &materials[model.material_id].descriptor_set, 0, nullptr);

        vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
}



} // namespace

int main() {
    return veekay::run({
        .init = initialize,
        .shutdown = shutdown,
        .update = update,
        .render = [](VkCommandBuffer cmd, VkFramebuffer framebuffer) {
            // ============================================
            // 1. НАЧАТЬ command buffer
            // ============================================
            vkResetCommandBuffer(cmd, 0);

            VkCommandBufferBeginInfo begin_info{};
            begin_info.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            begin_info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            vkBeginCommandBuffer(cmd, &begin_info);

            // ============================================
            // 2. SHADOW PASS (Dynamic Rendering)
            // ============================================
            render_shadow_pass(cmd);

            // ============================================
            // 3. MAIN PASS (Classic Render Pass)
            // ============================================
            render(cmd, framebuffer);

            // ============================================
            // 4. ЗАКОНЧИТЬ command buffer
            // ============================================
            vkEndCommandBuffer(cmd);
        },
    });
}

