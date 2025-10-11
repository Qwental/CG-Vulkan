#include <iostream>
#include <algorithm>
#include <fstream>
#include <vector>
#include <cmath>
#include <imgui.h>
#include <veekay/veekay.hpp>


#include <veekay/veekay.hpp>

// testbed/main.cpp

namespace {

constexpr float camera_near_plane = 0.01f;
constexpr float camera_far_plane = 100.0f;

struct Matrix {
    float m[4][4];
};

struct Vector {
    float x, y, z;
};

struct Vertex {
    Vector position;
    Vector color;  // Добавляем цветовой атрибут
};

// NOTE: Push constant для передачи данных в шейдеры
struct ShaderConstants {
    Matrix projection;
    Matrix transform;
};

struct VulkanBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
};

VkShaderModule vertex_shader_module;
VkShaderModule fragment_shader_module;
VkPipelineLayout pipeline_layout;
VkPipeline pipeline;

// NOTE: Буферы для цилиндра
VulkanBuffer vertex_buffer;
VulkanBuffer index_buffer;

// NOTE: Переменные для анимации и управления
Vector cylinder_position = {0.0f, 0.0f, 5.0f};
float animation_time = 0.0f;
float trajectory_radius = 3.0f;
bool enable_animation = true;

// NOTE: Параметры ортографической проекции
float ortho_left = -10.0f;
float ortho_right = 10.0f;
float ortho_bottom = -10.0f;
float ortho_top = 10.0f;

Matrix identity() {
    Matrix result{};
    result.m[0][0] = 1.0f;
    result.m[1][1] = 1.0f;
    result.m[2][2] = 1.0f;
    result.m[3][3] = 1.0f;
    return result;
}

Matrix orthographic_projection(float left, float right, float bottom, float top, float near, float far) {
    Matrix result{};
    result.m[0][0] = 2.0f / (right - left);
    result.m[1][1] = 2.0f / (top - bottom);
    result.m[2][2] = -2.0f / (far - near);
    result.m[3][0] = -(right + left) / (right - left);
    result.m[3][1] = -(top + bottom) / (top - bottom);
    result.m[3][2] = -(far + near) / (far - near);
    result.m[3][3] = 1.0f;
    return result;
}

Matrix translation(Vector vector) {
    Matrix result = identity();
    result.m[3][0] = vector.x;
    result.m[3][1] = vector.y;
    result.m[3][2] = vector.z;
    return result;
}

Matrix rotation(Vector axis, float angle) {
    Matrix result{};
    float length = sqrtf(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
    axis.x /= length;
    axis.y /= length;
    axis.z /= length;

    float sina = sinf(angle);
    float cosa = cosf(angle);
    float cosv = 1.0f - cosa;

    result.m[0][0] = (axis.x * axis.x * cosv) + cosa;
    result.m[0][1] = (axis.x * axis.y * cosv) + (axis.z * sina);
    result.m[0][2] = (axis.x * axis.z * cosv) - (axis.y * sina);
    result.m[1][0] = (axis.y * axis.x * cosv) - (axis.z * sina);
    result.m[1][1] = (axis.y * axis.y * cosv) + cosa;
    result.m[1][2] = (axis.y * axis.z * cosv) + (axis.x * sina);
    result.m[2][0] = (axis.z * axis.x * cosv) + (axis.y * sina);
    result.m[2][1] = (axis.z * axis.y * cosv) - (axis.x * sina);
    result.m[2][2] = (axis.z * axis.z * cosv) + cosa;
    result.m[3][3] = 1.0f;
    return result;
}

Matrix multiply(const Matrix& a, const Matrix& b) {
    Matrix result{};
    for (int j = 0; j < 4; j++) {
        for (int i = 0; i < 4; i++) {
            for (int k = 0; k < 4; k++) {
                result.m[j][i] += a.m[j][k] * b.m[k][i];
            }
        }
    }
    return result;
}

Vector circular_motion(double time, float radius) {
    float x = radius * cosf(time);
    float z = radius * sinf(time);
    return {x, 0.0f, z};
}

// NOTE: Загружает шейдерный модуль из файла
VkShaderModule loadShaderModule(const char* path) {
    // Открываем файл в бинарном режиме и сразу же проверяем
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Failed to open shader file: " << path << "\n";
        return VK_NULL_HANDLE;
    }

    // Получаем размер
    std::streamsize size = file.tellg();
    if (size <= 0 || size % 4 != 0) {
        std::cerr << "Invalid shader file size (" << size << "): " << path << "\n";
        return VK_NULL_HANDLE;
    }

    // Переходим в начало и читаем данные
    file.seekg(0, std::ios::beg);
    std::vector<uint32_t> buffer(static_cast<size_t>(size) / sizeof(uint32_t));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Failed to read shader file: " << path << "\n";
        return VK_NULL_HANDLE;
    }

    VkShaderModuleCreateInfo info{
        .sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = static_cast<size_t>(size),
        .pCode    = buffer.data(),
    };

    VkShaderModule module;
    if (vkCreateShaderModule(veekay::app.vk_device, &info, nullptr, &module) != VK_SUCCESS) {
        std::cerr << "vkCreateShaderModule failed for: " << path << "\n";
        return VK_NULL_HANDLE;
    }
    return module;
}

VulkanBuffer createBuffer(size_t size, void *data, VkBufferUsageFlags usage) {
    VkDevice& device = veekay::app.vk_device;
    VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;
    VulkanBuffer result{};

    // NOTE: Создаем буфер указанного размера и использования
    VkBufferCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO,
        .size = size,
        .usage = usage,
        .sharingMode = VK_SHARING_MODE_EXCLUSIVE,
    };
    if (vkCreateBuffer(device, &info, nullptr, &result.buffer) != VK_SUCCESS) {
        std::cerr << "Failed to create Vulkan buffer\n";
        return {};
    }

    // NOTE: Запрашиваем требования к памяти буфера
    VkMemoryRequirements requirements;
    vkGetBufferMemoryRequirements(device, result.buffer, &requirements);

    // NOTE: Запрашиваем типы памяти GPU
    VkPhysicalDeviceMemoryProperties properties;
    vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);

    // NOTE: Нужен тип памяти, видимый CPU и GPU с когерентностью
    const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                      VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

    // NOTE: Ищем подходящий тип памяти
    uint32_t index = UINT_MAX;
    for (uint32_t i = 0; i < properties.memoryTypeCount; ++i) {
        const VkMemoryType& type = properties.memoryTypes[i];
        if ((requirements.memoryTypeBits & (1 << i)) &&
            (type.propertyFlags & flags) == flags) {
            index = i;
            break;
        }
    }

    if (index == UINT_MAX) {
        std::cerr << "Failed to find required memory type to allocate Vulkan buffer\n";
        return {};
    }

    // NOTE: Выделяем память нужного размера и типа
    VkMemoryAllocateInfo allocInfo{
        .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
        .allocationSize = requirements.size,
        .memoryTypeIndex = index,
    };
    if (vkAllocateMemory(device, &allocInfo, nullptr, &result.memory) != VK_SUCCESS) {
        std::cerr << "Failed to allocate Vulkan buffer memory\n";
        return {};
    }

    // NOTE: Связываем память с буфером
    if (vkBindBufferMemory(device, result.buffer, result.memory, 0) != VK_SUCCESS) {
        std::cerr << "Failed to bind Vulkan buffer memory\n";
        return {};
    }

    // NOTE: Копируем данные в буфер
    void* device_data;
    vkMapMemory(device, result.memory, 0, requirements.size, 0, &device_data);
    memcpy(device_data, data, size);
    vkUnmapMemory(device, result.memory);

    return result;
}

void destroyBuffer(const VulkanBuffer& buffer) {
    VkDevice& device = veekay::app.vk_device;
    vkFreeMemory(device, buffer.memory, nullptr);
    vkDestroyBuffer(device, buffer.buffer, nullptr);
}

void initialize() {
    VkDevice& device = veekay::app.vk_device;

    { // NOTE: Создаем графический пайплайн
        vertex_shader_module = loadShaderModule("./shaders/cylinder.vert.spv");
        if (!vertex_shader_module) {
            std::cerr << "Failed to load Vulkan vertex shader from file\n";
            veekay::app.running = false;
            return;
        }

        fragment_shader_module = loadShaderModule("./shaders/cylinder.frag.spv");
        if (!fragment_shader_module) {
            std::cerr << "Failed to load Vulkan fragment shader from file\n";
            veekay::app.running = false;
            return;
        }

        VkPipelineShaderStageCreateInfo stage_infos[2];
        // NOTE: Вертексный шейдер
        stage_infos[0] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_shader_module,
            .pName = "main",
        };
        // NOTE: Фрагментный шейдер
        stage_infos[1] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_shader_module,
            .pName = "main",
        };

        // NOTE: Описание вершинного буфера
        VkVertexInputBindingDescription buffer_binding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };

        // NOTE: Описание атрибутов вершин
        VkVertexInputAttributeDescription attributes[] = {
            {
                .location = 0, // NOTE: Позиция
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, position),
            },
            {
                .location = 1, // NOTE: Цвет
                .binding = 0,
                .format = VK_FORMAT_R32G32B32_SFLOAT,
                .offset = offsetof(Vertex, color),
            },
        };

        VkPipelineVertexInputStateCreateInfo input_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
            .vertexBindingDescriptionCount = 1,
            .pVertexBindingDescriptions = &buffer_binding,
            .vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
            .pVertexAttributeDescriptions = attributes,
        };

        // NOTE: Каждые три вершины образуют треугольник
        VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };

        // NOTE: Настройки растеризации
        VkPipelineRasterizationStateCreateInfo raster_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1.0f,
        };

        // NOTE: Мультисемплинг - 1 семпл на пиксель
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

        VkPipelineViewportStateCreateInfo viewport_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO,
            .viewportCount = 1,
            .pViewports = &viewport,
            .scissorCount = 1,
            .pScissors = &scissor,
        };

        // NOTE: Тестирование глубины
        VkPipelineDepthStencilStateCreateInfo depth_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,
            .depthWriteEnable = true,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        };

        // NOTE: Запись всех цветовых каналов
        VkPipelineColorBlendAttachmentState attachment_info{
            .colorWriteMask = VK_COLOR_COMPONENT_R_BIT |
                            VK_COLOR_COMPONENT_G_BIT |
                            VK_COLOR_COMPONENT_B_BIT |
                            VK_COLOR_COMPONENT_A_BIT,
        };

        VkPipelineColorBlendStateCreateInfo blend_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,
            .logicOpEnable = false,
            .logicOp = VK_LOGIC_OP_COPY,
            .attachmentCount = 1,
            .pAttachments = &attachment_info
        };

        // NOTE: Push constants для передачи матриц
        VkPushConstantRange push_constants{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT,
            .size = sizeof(ShaderConstants),
        };

        VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        };

        // NOTE: Создаем layout пайплайна
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

        // NOTE: Создаем графический пайплайн
        if (vkCreateGraphicsPipelines(device, nullptr,
                                    1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan pipeline\n";
            veekay::app.running = false;
            return;
        }
    }

    // NOTE: Данные цилиндра - 48 вершин с цветами
    Vertex vertices[] = {
        {{ 1.000f, -1.000f, 0.000f }, { 1.000f, 0.500f, 0.300f }},
        {{ 1.000f, 1.000f, 0.000f }, { 1.000f, 0.500f, 1.000f }},
        {{ 0.966f, -1.000f, 0.259f }, { 0.983f, 0.629f, 0.300f }},
        {{ 0.966f, 1.000f, 0.259f }, { 0.983f, 0.629f, 1.000f }},
        {{ 0.866f, -1.000f, 0.500f }, { 0.933f, 0.750f, 0.300f }},
        {{ 0.866f, 1.000f, 0.500f }, { 0.933f, 0.750f, 1.000f }},
        {{ 0.707f, -1.000f, 0.707f }, { 0.854f, 0.854f, 0.300f }},
        {{ 0.707f, 1.000f, 0.707f }, { 0.854f, 0.854f, 1.000f }},
        {{ 0.500f, -1.000f, 0.866f }, { 0.750f, 0.933f, 0.300f }},
        {{ 0.500f, 1.000f, 0.866f }, { 0.750f, 0.933f, 1.000f }},
        {{ 0.259f, -1.000f, 0.966f }, { 0.629f, 0.983f, 0.300f }},
        {{ 0.259f, 1.000f, 0.966f }, { 0.629f, 0.983f, 1.000f }},
        {{ 0.000f, -1.000f, 1.000f }, { 0.500f, 1.000f, 0.300f }},
        {{ 0.000f, 1.000f, 1.000f }, { 0.500f, 1.000f, 1.000f }},
        {{ -0.259f, -1.000f, 0.966f }, { 0.371f, 0.983f, 0.300f }},
        {{ -0.259f, 1.000f, 0.966f }, { 0.371f, 0.983f, 1.000f }},
        {{ -0.500f, -1.000f, 0.866f }, { 0.250f, 0.933f, 0.300f }},
        {{ -0.500f, 1.000f, 0.866f }, { 0.250f, 0.933f, 1.000f }},
        {{ -0.707f, -1.000f, 0.707f }, { 0.146f, 0.854f, 0.300f }},
        {{ -0.707f, 1.000f, 0.707f }, { 0.146f, 0.854f, 1.000f }},
        {{ -0.866f, -1.000f, 0.500f }, { 0.067f, 0.750f, 0.300f }},
        {{ -0.866f, 1.000f, 0.500f }, { 0.067f, 0.750f, 1.000f }},
        {{ -0.966f, -1.000f, 0.259f }, { 0.017f, 0.629f, 0.300f }},
        {{ -0.966f, 1.000f, 0.259f }, { 0.017f, 0.629f, 1.000f }},
        {{ -1.000f, -1.000f, 0.000f }, { 0.000f, 0.500f, 0.300f }},
        {{ -1.000f, 1.000f, 0.000f }, { 0.000f, 0.500f, 1.000f }},
        {{ -0.966f, -1.000f, -0.259f }, { 0.017f, 0.371f, 0.300f }},
        {{ -0.966f, 1.000f, -0.259f }, { 0.017f, 0.371f, 1.000f }},
        {{ -0.866f, -1.000f, -0.500f }, { 0.067f, 0.250f, 0.300f }},
        {{ -0.866f, 1.000f, -0.500f }, { 0.067f, 0.250f, 1.000f }},
        {{ -0.707f, -1.000f, -0.707f }, { 0.146f, 0.146f, 0.300f }},
        {{ -0.707f, 1.000f, -0.707f }, { 0.146f, 0.146f, 1.000f }},
        {{ -0.500f, -1.000f, -0.866f }, { 0.250f, 0.067f, 0.300f }},
        {{ -0.500f, 1.000f, -0.866f }, { 0.250f, 0.067f, 1.000f }},
        {{ -0.259f, -1.000f, -0.966f }, { 0.371f, 0.017f, 0.300f }},
        {{ -0.259f, 1.000f, -0.966f }, { 0.371f, 0.017f, 1.000f }},
        {{ -0.000f, -1.000f, -1.000f }, { 0.500f, 0.000f, 0.300f }},
        {{ -0.000f, 1.000f, -1.000f }, { 0.500f, 0.000f, 1.000f }},
        {{ 0.259f, -1.000f, -0.966f }, { 0.629f, 0.017f, 0.300f }},
        {{ 0.259f, 1.000f, -0.966f }, { 0.629f, 0.017f, 1.000f }},
        {{ 0.500f, -1.000f, -0.866f }, { 0.750f, 0.067f, 0.300f }},
        {{ 0.500f, 1.000f, -0.866f }, { 0.750f, 0.067f, 1.000f }},
        {{ 0.707f, -1.000f, -0.707f }, { 0.854f, 0.146f, 0.300f }},
        {{ 0.707f, 1.000f, -0.707f }, { 0.854f, 0.146f, 1.000f }},
        {{ 0.866f, -1.000f, -0.500f }, { 0.933f, 0.250f, 0.300f }},
        {{ 0.866f, 1.000f, -0.500f }, { 0.933f, 0.250f, 1.000f }},
        {{ 0.966f, -1.000f, -0.259f }, { 0.983f, 0.371f, 0.300f }},
        {{ 0.966f, 1.000f, -0.259f }, { 0.983f, 0.371f, 1.000f }},
    };

    uint32_t indices[] = {
        0, 1, 2, 1, 3, 2, 2, 3, 4, 3, 5, 4,
        4, 5, 6, 5, 7, 6, 6, 7, 8, 7, 9, 8,
        8, 9, 10, 9, 11, 10, 10, 11, 12, 11, 13, 12,
        12, 13, 14, 13, 15, 14, 14, 15, 16, 15, 17, 16,
        16, 17, 18, 17, 19, 18, 18, 19, 20, 19, 21, 20,
        20, 21, 22, 21, 23, 22, 22, 23, 24, 23, 25, 24,
        24, 25, 26, 25, 27, 26, 26, 27, 28, 27, 29, 28,
        28, 29, 30, 29, 31, 30, 30, 31, 32, 31, 33, 32,
        32, 33, 34, 33, 35, 34, 34, 35, 36, 35, 37, 36,
        36, 37, 38, 37, 39, 38, 38, 39, 40, 39, 41, 40,
        40, 41, 42, 41, 43, 42, 42, 43, 44, 43, 45, 44,
        44, 45, 46, 45, 47, 46, 46, 47, 0, 47, 1, 0,
    };

    vertex_buffer = createBuffer(sizeof(vertices), vertices,
                               VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    index_buffer = createBuffer(sizeof(indices), indices,
                              VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void shutdown() {
    VkDevice& device = veekay::app.vk_device;

    // NOTE: Освобождаем ресурсы
    destroyBuffer(index_buffer);
    destroyBuffer(vertex_buffer);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyShaderModule(device, fragment_shader_module, nullptr);
    vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {
    // NOTE: Интерфейс ImGui для управления
    ImGui::Begin("Cylinder Controls");

    ImGui::Checkbox("Enable Animation", &enable_animation);
    ImGui::SliderFloat("Trajectory Radius", &trajectory_radius, 0.5f, 10.0f);

    ImGui::Separator();
    ImGui::Text("Orthographic Projection");
    ImGui::SliderFloat("Left", &ortho_left, -20.0f, 0.0f);
    ImGui::SliderFloat("Right", &ortho_right, 0.0f, 20.0f);
    ImGui::SliderFloat("Bottom", &ortho_bottom, -20.0f, 0.0f);
    ImGui::SliderFloat("Top", &ortho_top, 0.0f, 20.0f);

    ImGui::Separator();
    ImGui::InputFloat3("Manual Position", reinterpret_cast<float*>(&cylinder_position));

    ImGui::End();

    // NOTE: Обновляем позицию цилиндра
    if (enable_animation) {
        Vector motion = circular_motion(time * 0.5, trajectory_radius);
        cylinder_position.x = motion.x;
        cylinder_position.z = motion.z;
        animation_time = float(time);
    }
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);

    { // NOTE: Начинаем запись команд
        VkCommandBufferBeginInfo info{
            .sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO,
            .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT,
        };
        vkBeginCommandBuffer(cmd, &info);
    }

    { // NOTE: Начинаем render pass с очисткой
        VkClearValue clear_color{.color = {{0.2f, 0.3f, 0.4f, 1.0f}}};
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

    // NOTE: Используем наш графический пайплайн
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // NOTE: Привязываем вершинный и индексный буферы
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer.buffer, offset, VK_INDEX_TYPE_UINT32);

    // NOTE: Создаем матрицы трансформации
    float aspect_ratio = float(veekay::app.window_width) / float(veekay::app.window_height);

    // NOTE: Используем ортографическую проекцию с пропорциональными размерами
    float ortho_left_adj = ortho_left * aspect_ratio;
    float ortho_right_adj = ortho_right * aspect_ratio;

    ShaderConstants constants{
        .projection = orthographic_projection(
            ortho_left_adj, ortho_right_adj,
            ortho_bottom, ortho_top,
            camera_near_plane, camera_far_plane),
        .transform = multiply(
            rotation({0.0f, 1.0f, 0.0f}, animation_time * 0.5f),
            translation(cylinder_position)
        ),
    };

    // NOTE: Передаем константы в шейдер
    vkCmdPushConstants(cmd, pipeline_layout,
                     VK_SHADER_STAGE_VERTEX_BIT,
                     0, sizeof(ShaderConstants), &constants);

    // NOTE: Отрисовываем цилиндр (144 индекса = 48 треугольников)
    vkCmdDrawIndexed(cmd, 144, 1, 0, 0, 0);

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
