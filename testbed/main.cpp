#include <cstdint>
#include <climits>
#include <cstring>
#include <vector>
#include <iostream>
#include <fstream>
#include <cmath>

#include <veekay/veekay.hpp>

#include <vulkan/vulkan_core.h>
#include <imgui.h>
#include <lodepng.h>

namespace {

float animation_time = 0.0f;
float animation_speed = 1.0f;
float trajectory_radius = 1.5f;
bool animation_paused = false;
float pause_time = 0.0f;
int animation_direction = 1;
int cylinder_model_index = 2;  // NOTE: Теперь третий (0-куб, 1-цилиндр)

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
    float _pad2;
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

    std::vector<Model> models;
}

// NOTE: Vulkan objects
inline namespace {
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
    VkSampler texture_sampler;
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

veekay::mat4 Transform::matrix() const {
    // TODO: Scaling and rotation
    auto t = veekay::mat4::translation(position);
    return t;
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

void initialize(VkCommandBuffer cmd) {
    VkDevice& device = veekay::app.vk_device;
    VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;

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
                .maxSets = 1,
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

        // NOTE: Declare external data sources
        VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .setLayoutCount = 1,
            .pSetLayouts = &descriptor_set_layout,
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
        };

        vkUpdateDescriptorSets(device, std::size(write_infos),
                             write_infos, 0, nullptr);
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

    // NOTE: Add models to scene
    models.emplace_back(Model{
        .mesh = plane_mesh,
        .transform = Transform{.position = {0.0f, 3.0f, 0.0f}},
        .albedo_color = veekay::vec3{1.0f, 1.0f, 1.0f}
    });

    // NOTE: КУБА
    models.emplace_back(Model{
        .mesh = cube_mesh,
        .transform = Transform{
            .position = {2.0f, -0.5f, 0.0f},
            .scale = {0.8f, 0.8f, 0.8f},
        },
        .albedo_color = veekay::vec3{0.9f, 0.2f, 0.2f}
    });

    // NOTE: ЦИЛИНДР
    models.emplace_back(Model{
        .mesh = cylinder_mesh,
        .transform = Transform{
            .position = {0.0f, -0.0f, 0.0f},
        },
        .albedo_color = veekay::vec3{0.8f, 0.8f, 0.8f}
    });
}

// NOTE: Destroy resources here
void shutdown() {
    VkDevice& device = veekay::app.vk_device;

    vkDestroySampler(device, missing_texture_sampler, nullptr);
    delete missing_texture;

    delete cylinder_mesh.index_buffer;
    delete cylinder_mesh.vertex_buffer;

    delete cube_mesh.index_buffer;
    delete cube_mesh.vertex_buffer;

    delete plane_mesh.index_buffer;
    delete plane_mesh.vertex_buffer;

    delete model_uniforms_buffer;
    delete scene_uniforms_buffer;
    delete spot_lights_buffer;

    vkDestroyDescriptorSetLayout(device, descriptor_set_layout, nullptr);
    vkDestroyDescriptorPool(device, descriptor_pool, nullptr);

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

    // NOTE: Применяем позицию к цилиндру
    models[cylinder_model_index].transform.position = {x, 0.0f, z};

    // NOTE: Self-rotation цилиндра вокруг оси Y
    models[cylinder_model_index].transform.rotation.y += 0.01f;

    // NOTE: Обработка камеры и управления
    if (!ImGui::IsWindowHovered()) {
        using namespace veekay::input;

        if (mouse::isButtonDown(mouse::Button::left)) {
            // NOTE: Получаем смещение мыши в пикселях
            auto move_delta = mouse::cursorDelta();

            // NOTE: Обновляем pitch/yaw на основе движения мыши (FPS style)
            camera.yaw -= move_delta.x * camera.mouse_sensitivity;
            camera.pitch -= move_delta.y * camera.mouse_sensitivity;

            if (camera.pitch > 89.0f)  camera.pitch = 89.0f;
            if (camera.pitch < -89.0f) camera.pitch = -89.0f;

            // NOTE: Вычисляем базисные векторы из текущих pitch/yaw
            CameraBasis basis = compute_camera_basis(camera.pitch, camera.yaw);

            // NOTE: Получаем вектора для движения
            veekay::vec3 forward = basis.forward;
            veekay::vec3 right = basis.right;
            veekay::vec3 up = {0.0f, 1.0f, 0.0f};

            // NOTE: Обработка WASD для движения
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

    // NOTE: Заполняем SceneUniforms для текущего кадра
    SceneUniforms scene_uniforms{
        .view_projection = camera.view_projection(aspect_ratio),
        .view_position = camera.position,
        .ambient_light_intensity = lighting_params.ambient_color,
        .sun_light_direction = normalize(lighting_params.directional_direction),
        .sun_light_color = lighting_params.directional_color,
        .spot_lights_count = 1,
    };

    // NOTE: Копируем в GPU буфер
    *(SceneUniforms*)scene_uniforms_buffer->mapped_region = scene_uniforms;

    // NOTE: Позиция куба (на нём прожектор)
    veekay::vec3 cube_position = models[1].transform.position;

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

    // NOTE: Заполняем ModelUniforms для каждого объекта
    std::vector<ModelUniforms> model_uniforms(models.size());
    for (size_t i = 0, n = models.size(); i < n; ++i) {
        const Model& model = models[i];
        ModelUniforms& uniforms = model_uniforms[i];

        // NOTE: Матрица трансформации объекта
        uniforms.model = model.transform.matrix();

        // NOTE: Диффузный цвет (альбедо)
        uniforms.albedo_color = model.albedo_color;

        // NOTE: Устанавливаем параметры материала в зависимости от объекта
        if (i == 0) {
            // NOTE: Плоскость: матовый материал
            uniforms.specular_color = {0.3f, 0.3f, 0.3f};
            uniforms.shininess = 16.0f;
        }
        else if (i == 1) {
            // NOTE: Куб: блестящий материал
            uniforms.specular_color = {1.0f, 1.0f, 1.0f};
            uniforms.shininess = 128.0f;
        }
        else if (i == 2) {
            // NOTE: Цилиндр: глянцевый материал
            uniforms.specular_color = {1.0f, 1.0f, 1.0f};
            uniforms.shininess = 64.0f;
        }
        else {
            // NOTE: Остальные объекты: средние параметры
            uniforms.specular_color = {0.7f, 0.7f, 0.7f};
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

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);

    {
        VkCommandBufferBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };

        vkBeginCommandBuffer(cmd, &info);
    }

    {
        VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
        VkClearValue clear_depth{.depthStencil = {1.0f, 0}};

        VkClearValue clear_values[] = {clear_color, clear_depth};

        VkRenderPassBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO,
            .renderPass = veekay::app.vk_render_pass,
            .framebuffer = framebuffer,
            .renderArea = {
                .extent = {
                    veekay::app.window_width,
                    veekay::app.window_height
                },
            },
            .clearValueCount = 2,
            .pClearValues = clear_values,
        };

        vkCmdBeginRenderPass(cmd, &info, VK_SUBPASS_CONTENTS_INLINE);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkDeviceSize zero_offset = 0;

    VkBuffer current_vertex_buffer = VK_NULL_HANDLE;
    VkBuffer current_index_buffer = VK_NULL_HANDLE;

    const size_t model_uniforms_alignment =
        veekay::graphics::Buffer::structureAlignment(sizeof(ModelUniforms));

    for (size_t i = 0, n = models.size(); i < n; ++i) {
        const Model& model = models[i];
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

        vkCmdDrawIndexed(cmd, mesh.indices, 1, 0, 0, 0);
    }

    vkCmdEndRenderPass(cmd);
    vkEndCommandBuffer(cmd);
}

} // namespace

int main() {
    return veekay::run({
        .init = initialize,
        .shutdown = shutdown,
        .update = update,
        .render = render,
    });
}
