#include <cstdint>
#include <climits>
#include <vector>
#include <iostream>
#include <fstream>
#include <numbers>
#include <cmath>

#include <veekay/veekay.hpp>

#include <imgui.h>
#include <vulkan/vulkan_core.h>

namespace {

constexpr float camera_fov = 70.0f;
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
    Vector color;
};

struct ShaderConstants {
    Matrix projection;
    Matrix transform;
    Vector color;
};

struct VulkanBuffer {
    VkBuffer buffer;
    VkDeviceMemory memory;
};

VkShaderModule vertex_shader_module;
VkShaderModule fragment_shader_module;
VkPipelineLayout pipeline_layout;
VkPipeline pipeline;

VulkanBuffer vertex_buffer;
VulkanBuffer index_buffer;

Vector model_position = {0.0f, 0.0f, 5.0f};  // ИЗМЕНЕНО: начальная позиция как в рабочем коде
float model_rotation;
Vector model_color = {0.5f, 1.0f, 0.7f};
bool model_spin = true;
float trajectory_radius = 3.0f;
float trajectory_speed = 1.0f;
float trajectory_angle = 0.0f;
bool animation_paused = false;
bool animation_reversed = false;
float animation_direction = 1.0f;
float rotation_speed = 1.0f;

Matrix identity() {
    Matrix result{};
    result.m[0][0] = 1.0f;
    result.m[1][1] = 1.0f;
    result.m[2][2] = 1.0f;
    result.m[3][3] = 1.0f;
    return result;
}

Matrix projection(float fov, float aspect_ratio, float near, float far) {
    Matrix result{};
    const float radians = fov * std::numbers::pi / 180.0f;
    const float cot = 1.0f / tanf(radians / 2.0f);
    result.m[0][0] = cot / aspect_ratio;
    result.m[1][1] = cot;
    result.m[2][3] = 1.0f;
    result.m[2][2] = far / (far - near);
    result.m[3][2] = (-near * far) / (far - near);
    return result;
}

Matrix translation(Vector vector) {
    Matrix result = identity();
    result.m[3][0] = vector.x;
    result.m[3][1] = vector.y;
    result.m[3][2] = vector.z;
    return result;
}

Matrix orthographic(float left, float right, float bottom, float top, float near, float far) {
    Matrix result = identity();
    result.m[0][0] = 2.0f / (right - left);
    result.m[1][1] = 2.0f / (top - bottom);
    result.m[2][2] = 1.0f / (far - near);
    result.m[3][0] = -(right + left) / (right - left);
    result.m[3][1] = -(top + bottom) / (top - bottom);
    result.m[3][2] = -near / (far - near);
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

VkShaderModule loadShaderModule(const char* path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file.is_open()) {
        std::cerr << "Shader open failed: " << path << "\n";
        return VK_NULL_HANDLE;
    }
    std::streampos end = file.tellg();
    if (end <= 0) {
        std::cerr << "Shader size invalid: " << path << "\n";
        return VK_NULL_HANDLE;
    }
    size_t size = static_cast<size_t>(end);
    if ((size % 4) != 0) {
        std::cerr << "Shader size not multiple of 4: " << size << " for " << path << "\n";
        return VK_NULL_HANDLE;
    }
    file.seekg(0, std::ios::beg);
    std::vector<uint32_t> buffer(size / sizeof(uint32_t));
    if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
        std::cerr << "Shader read failed: " << path << "\n";
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{
        .sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
        .codeSize = size,
        .pCode = buffer.data(),
    };
    VkShaderModule module = VK_NULL_HANDLE;
    if (vkCreateShaderModule(veekay::app.vk_device, &info, nullptr, &module) != VK_SUCCESS) {
        std::cerr << "vkCreateShaderModule failed: " << path << "\n";
        return VK_NULL_HANDLE;
    }
    return module;
}

VulkanBuffer createBuffer(size_t size, void *data, VkBufferUsageFlags usage) {
    VkDevice& device = veekay::app.vk_device;
    VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;
    VulkanBuffer result{};
    {
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
    }
    {
        VkMemoryRequirements requirements;
        vkGetBufferMemoryRequirements(device, result.buffer, &requirements);
        VkPhysicalDeviceMemoryProperties properties;
        vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);
        const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                            VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;
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
        VkMemoryAllocateInfo info{
            .sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
            .allocationSize = requirements.size,
            .memoryTypeIndex = index,
        };
        if (vkAllocateMemory(device, &info, nullptr, &result.memory) != VK_SUCCESS) {
            std::cerr << "Failed to allocate Vulkan buffer memory\n";
            return {};
        }
        if (vkBindBufferMemory(device, result.buffer, result.memory, 0) != VK_SUCCESS) {
            std::cerr << "Failed to bind Vulkan buffer memory\n";
            return {};
        }
        void* device_data;
        vkMapMemory(device, result.memory, 0, requirements.size, 0, &device_data);
        memcpy(device_data, data, size);
        vkUnmapMemory(device, result.memory);
    }
    return result;
}

void destroyBuffer(const VulkanBuffer& buffer) {
    VkDevice& device = veekay::app.vk_device;
    vkFreeMemory(device, buffer.memory, nullptr);
    vkDestroyBuffer(device, buffer.buffer, nullptr);
}

void initialize() {
    VkDevice& device = veekay::app.vk_device;
    VkPhysicalDevice& physical_device = veekay::app.vk_physical_device;

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
        stage_infos[0] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_VERTEX_BIT,
            .module = vertex_shader_module,
            .pName = "main",
        };
        stage_infos[1] = VkPipelineShaderStageCreateInfo{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO,
            .stage = VK_SHADER_STAGE_FRAGMENT_BIT,
            .module = fragment_shader_module,
            .pName = "main",
        };

        VkVertexInputBindingDescription buffer_binding{
            .binding = 0,
            .stride = sizeof(Vertex),
            .inputRate = VK_VERTEX_INPUT_RATE_VERTEX,
        };

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

        VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
            .topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
        };

        VkPipelineRasterizationStateCreateInfo raster_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
            .polygonMode = VK_POLYGON_MODE_FILL,
            .cullMode = VK_CULL_MODE_BACK_BIT,
            .frontFace = VK_FRONT_FACE_CLOCKWISE,
            .lineWidth = 1.0f,
        };

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

        VkPipelineDepthStencilStateCreateInfo depth_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO,
            .depthTestEnable = true,
            .depthWriteEnable = true,
            .depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL,
        };

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

        VkPushConstantRange push_constants{
            .stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
            .size = sizeof(ShaderConstants),
        };

        VkPipelineLayoutCreateInfo layout_info{
            .sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push_constants,
        };

        if (vkCreatePipelineLayout(device, &layout_info, nullptr, &pipeline_layout) != VK_SUCCESS) {
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

        if (vkCreateGraphicsPipelines(device, nullptr, 1, &info, nullptr, &pipeline) != VK_SUCCESS) {
            std::cerr << "Failed to create Vulkan pipeline\n";
            veekay::app.running = false;
            return;
        }
    }

    const int segments = 8;
    const float height = 2.0f;
    const float radius = 0.5f;

    std::vector<Vertex> vertices;
    std::vector<uint32_t> indices;

    for (int i = 0; i < segments; ++i) {
        float angle1 = 2.0f * std::numbers::pi * i / segments;
        float angle2 = 2.0f * std::numbers::pi * (i + 1) / segments;

        float x1 = cosf(angle1) * radius;
        float z1 = sinf(angle1) * radius;
        float x2 = cosf(angle2) * radius;
        float z2 = sinf(angle2) * radius;

        Vector white_color = {1.0f, 1.0f, 1.0f};
        Vector blue_color = {0.0f, 0.4f, 1.0f};

        vertices.push_back({{x1, -height/2, z1}, blue_color});
        vertices.push_back({{x1, height/2, z1}, white_color});
        vertices.push_back({{x2, -height/2, z2}, blue_color});

        vertices.push_back({{x1, height/2, z1}, white_color});
        vertices.push_back({{x2, height/2, z2}, white_color});
        vertices.push_back({{x2, -height/2, z2}, blue_color});
    }

    for (uint32_t i = 0; i < segments * 6; ++i) {
        indices.push_back(i);
    }

    vertex_buffer = createBuffer(vertices.size() * sizeof(Vertex), vertices.data(),
                                  VK_BUFFER_USAGE_VERTEX_BUFFER_BIT);
    index_buffer = createBuffer(indices.size() * sizeof(uint32_t), indices.data(),
                                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT);
}

void shutdown() {
    VkDevice& device = veekay::app.vk_device;
    destroyBuffer(index_buffer);
    destroyBuffer(vertex_buffer);
    vkDestroyPipeline(device, pipeline, nullptr);
    vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
    vkDestroyShaderModule(device, fragment_shader_module, nullptr);
    vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

void update(double time) {
    ImGui::Begin("Controls:");
    ImGui::SliderFloat("Trajectory Radius", &trajectory_radius, 1.0f, 10.0f);
    ImGui::SliderFloat("Trajectory Speed", &trajectory_speed, 0.1f, 5.0f);
    ImGui::SliderFloat("Rotation Speed", &rotation_speed, 0.1f, 5.0f);
    ImGui::Checkbox("Spin (Self Rotation)", &model_spin);

    if (ImGui::Button(animation_paused ? "Resume Orbit" : "Pause Orbit")) {
        animation_paused = !animation_paused;
    }
    if (ImGui::Button(animation_reversed ? "Normal Direction" : "Reverse Direction")) {
        animation_reversed = !animation_reversed;
        animation_direction = animation_reversed ? -1.0f : 1.0f;
    }

    ImGui::Text("Orbit: %s %s",
                animation_paused ? "PAUSED" : "RUNNING",
                animation_reversed ? "(REVERSED)" : "(NORMAL)");
    ImGui::Text("Self Rotation: %s", model_spin ? "ON" : "OFF");
    ImGui::End();

    if (!animation_paused) {
        trajectory_angle = time * trajectory_speed * animation_direction;
        model_position.x = cosf(trajectory_angle) * trajectory_radius;
        model_position.y = 0.0f;
        model_position.z = 5.0f + sinf(trajectory_angle) * trajectory_radius;  // ИЗМЕНЕНО: 5.0f базовая позиция
    }

    if (model_spin) {
        model_rotation = time * rotation_speed;
    }

    trajectory_angle = fmodf(trajectory_angle, 2.0f * std::numbers::pi);
    model_rotation = fmodf(model_rotation, 2.0f * std::numbers::pi);
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);

    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
    if (fbScale.x <= 0.0f) fbScale.x = 1.0f;
    if (fbScale.y <= 0.0f) fbScale.y = 1.0f;

    const uint32_t fbw = static_cast<uint32_t>(std::round(veekay::app.window_width * fbScale.x));
    const uint32_t fbh = static_cast<uint32_t>(std::round(veekay::app.window_height * fbScale.y));

    VkClearValue clear_color{.color = {{0.1f, 0.1f, 0.1f, 1.0f}}};
    VkClearValue clear_depth{.depthStencil = {1.0f, 0}};
    VkClearValue clears[] = {clear_color, clear_depth};

    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = veekay::app.vk_render_pass;
    rp.framebuffer = framebuffer;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = {fbw, fbh};
    rp.clearValueCount = 2;
    rp.pClearValues = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

    // ИЗМЕНЕНО: используем ортографическую проекцию и правильный transform
    ShaderConstants constants{
        .projection = orthographic(
            -5.0f, 5.0f,    // left, right
            -5.0f, 5.0f,    // bottom, top
            camera_near_plane, camera_far_plane
        ),
        .transform = multiply(
            multiply(rotation({0.0f, 1.0f, 0.0f}, model_rotation),
                     rotation({1.0f, 0.0f, 0.0f}, -0.5f)),
            translation(model_position)
        ),
        .color = model_color,
    };

    vkCmdPushConstants(cmd, pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(ShaderConstants), &constants);

    vkCmdDrawIndexed(cmd, 8 * 6, 1, 0, 0, 0);

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
