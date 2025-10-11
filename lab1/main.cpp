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
	// NOTE: You can add more attributes
};

// NOTE: These variable will be available to shaders through push constant uniform
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

// NOTE: Declare buffers and other variables here
VulkanBuffer vertex_buffer;
VulkanBuffer index_buffer;

Vector model_position = {0.0f, 0.0f, 0.0f};
float model_rotation;
Vector model_color = {0.5f, 1.0f, 0.7f };
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

// NOTE: Loads shader byte code from file
// NOTE: Your shaders are compiled via CMake with this code too, look it up
	VkShaderModule loadShaderModule(const char* path) {
	// 1) Открытие файла
	std::ifstream file(path, std::ios::binary | std::ios::ate);
	if (!file.is_open()) {
		std::cerr << "Shader open failed: " << path << "\n";
		return VK_NULL_HANDLE;
	}

	// 2) Получение размера и валидация
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

	// 3) Чтение данных
	file.seekg(0, std::ios::beg);
	std::vector<uint32_t> buffer(size / sizeof(uint32_t));
	if (!file.read(reinterpret_cast<char*>(buffer.data()), size)) {
		std::cerr << "Shader read failed: " << path << "\n";
		return VK_NULL_HANDLE;
	}

	// 4) Создание модуля
	VkShaderModuleCreateInfo info{
		.sType    = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO,
		.codeSize = size,
		.pCode    = buffer.data(),
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
		// NOTE: We create a buffer of specific usage with specified size
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

	// NOTE: Creating a buffer does not allocate memory,
	//       only a buffer **object** was created.
	//       So, we allocate memory for the buffer

	{
		// NOTE: Ask buffer about its memory requirements
		VkMemoryRequirements requirements;
		vkGetBufferMemoryRequirements(device, result.buffer, &requirements);

		// NOTE: Ask GPU about types of memory it supports
		VkPhysicalDeviceMemoryProperties properties;
		vkGetPhysicalDeviceMemoryProperties(physical_device, &properties);

		// NOTE: We want type of memory which is visible to both CPU and GPU
		// NOTE: HOST is CPU, DEVICE is GPU; we are interested in "CPU" visible memory
		// NOTE: COHERENT means that CPU cache will be invalidated upon mapping memory region
		const VkMemoryPropertyFlags flags = VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
		                                    VK_MEMORY_PROPERTY_HOST_COHERENT_BIT;

		// NOTE: Linear search through types of memory until
		//       one type matches the requirements, thats the index of memory type
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

		// NOTE: Allocate required memory amount in appropriate memory type
		VkMemoryAllocateInfo info{
			.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO,
			.allocationSize = requirements.size,
			.memoryTypeIndex = index,
		};

		if (vkAllocateMemory(device, &info, nullptr, &result.memory) != VK_SUCCESS) {
			std::cerr << "Failed to allocate Vulkan buffer memory\n";
			return {};
		}

		// NOTE: Link allocated memory with a buffer
		if (vkBindBufferMemory(device, result.buffer, result.memory, 0) != VK_SUCCESS) {
			std::cerr << "Failed to bind Vulkan  buffer memory\n";
			return {};
		}

		// NOTE: Get pointer to allocated memory
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

	{ // NOTE: Build graphics pipeline
vertex_shader_module   = loadShaderModule("./shaders/shader.vert.spv");
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
				.location = 0, // NOTE: First attribute
				.binding = 0, // NOTE: First vertex buffer
				.format = VK_FORMAT_R32G32B32_SFLOAT, // NOTE: 3-component vector of floats
				.offset = offsetof(Vertex, position), // NOTE: Offset of "position" field in a Vertex struct
			},
				{
					.location = 1, // NOTE: Second attribute - color
					.binding = 0,
					.format = VK_FORMAT_R32G32B32_SFLOAT,
					.offset = offsetof(Vertex, color),
				},

			// NOTE: If you want more attributes per vertex, declare them here
#if 0
			{
				.location = 1, // NOTE: Second attribute
				.binding = 0,
				.format = VK_FORMAT_XXX,
				.offset = offset(Vertex, your_attribute),
			},
#endif
		};

		// NOTE: Bring
		VkPipelineVertexInputStateCreateInfo input_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO,
			.vertexBindingDescriptionCount = 1,
			.pVertexBindingDescriptions = &buffer_binding,
			.vertexAttributeDescriptionCount = sizeof(attributes) / sizeof(attributes[0]),
			.pVertexAttributeDescriptions = attributes,
		};

		// NOTE: Every three vertices make up a triangle,
		//       so our vertex buffer contains a "list of triangles"
		VkPipelineInputAssemblyStateCreateInfo assembly_state_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO,
			.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST,
		};

		// NOTE: Declare clockwise triangle order as front-facing
		//       Discard triangles that are facing away
		//       Fill triangles, don't draw lines instaed
		VkPipelineRasterizationStateCreateInfo raster_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO,
			.polygonMode = VK_POLYGON_MODE_FILL,
			.cullMode = VK_CULL_MODE_BACK_BIT,
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

		// NOTE: Let rasterizer perform depth-testing and overwrite depth values on condition pass
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

		// NOTE: Let rasterizer just copy resulting pixels onto a buffer, don't blend
		VkPipelineColorBlendStateCreateInfo blend_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO,

			.logicOpEnable = false,
			.logicOp = VK_LOGIC_OP_COPY,

			.attachmentCount = 1,
			.pAttachments = &attachment_info
		};

		// NOTE: Declare constant memory region visible to vertex and fragment shaders
		VkPushConstantRange push_constants{
			.stageFlags = VK_SHADER_STAGE_VERTEX_BIT |
			              VK_SHADER_STAGE_FRAGMENT_BIT,
			.size = sizeof(ShaderConstants),
		};

		// NOTE: Declare external data sources, only push constants this time
		VkPipelineLayoutCreateInfo layout_info{
			.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO,
			.pushConstantRangeCount = 1,
			.pPushConstantRanges = &push_constants,
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

	// TODO: You define model vertices and create buffers here
	// TODO: Index buffer has to be created here too
	// NOTE: Look for createBuffer function

	const int segments = 8;
	const float height = 2.0f;
	const float radius = 0.5f;

	std::vector<Vertex> vertices;
	std::vector<uint32_t> indices;

	// Генерация боковой поверхности цилиндра
	// Генерация боковой поверхности цилиндра
	for (int i = 0; i < segments; ++i) {
		float angle1 = 2.0f * std::numbers::pi * i / segments;
		float angle2 = 2.0f * std::numbers::pi * (i + 1) / segments;

		// Координаты для текущего сегмента
		float x1 = cosf(angle1) * radius;
		float z1 = sinf(angle1) * radius;
		float x2 = cosf(angle2) * radius;
		float z2 = sinf(angle2) * radius;

		// Белый цвет для верхних вершин, синий для нижних
		Vector white_color = {1.0f, 1.0f, 1.0f};  // Белый
		Vector blue_color = {0.0f, 0.4f, 1.0f};   // Синий

		// Первый треугольник: bottom1 -> top1 -> bottom2
		vertices.push_back({{x1, -height/2, z1}, blue_color});  // bottom1 - синий
		vertices.push_back({{x1, height/2, z1}, white_color});  // top1 - белый
		vertices.push_back({{x2, -height/2, z2}, blue_color});  // bottom2 - синий

		// Второй треугольник: top1 -> top2 -> bottom2
		vertices.push_back({{x1, height/2, z1}, white_color});  // top1 - белый
		vertices.push_back({{x2, height/2, z2}, white_color});  // top2 - белый
		vertices.push_back({{x2, -height/2, z2}, blue_color});  // bottom2 - синий
	}


	// Создаем индексы (0, 1, 2, 3, 4, 5, ...)
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

	// NOTE: Destroy resources here, do not cause leaks in your program!
	destroyBuffer(index_buffer);
	destroyBuffer(vertex_buffer);

	vkDestroyPipeline(device, pipeline, nullptr);
	vkDestroyPipelineLayout(device, pipeline_layout, nullptr);
	vkDestroyShaderModule(device, fragment_shader_module, nullptr);
	vkDestroyShaderModule(device, vertex_shader_module, nullptr);
}

	void update(double time) {
	ImGui::Begin("Controls:");

	// Управление траекторией
	ImGui::SliderFloat("Trajectory Radius", &trajectory_radius, 1.0f, 10.0f);
	ImGui::SliderFloat("Trajectory Speed", &trajectory_speed, 0.1f, 5.0f);

	// Управление вращением вокруг своей оси
	ImGui::SliderFloat("Rotation Speed", &rotation_speed, 0.1f, 5.0f);
	ImGui::Checkbox("Spin (Self Rotation)", &model_spin);

	// Управление анимацией движения по кругу
	if (ImGui::Button(animation_paused ? "Resume Orbit" : "Pause Orbit")) {
		animation_paused = !animation_paused;
	}

	if (ImGui::Button(animation_reversed ? "Normal Direction" : "Reverse Direction")) {
		animation_reversed = !animation_reversed;
		animation_direction = animation_reversed ? -1.0f : 1.0f;
	}

	// Отображение текущего состояния
	ImGui::Text("Orbit: %s %s",
				animation_paused ? "PAUSED" : "RUNNING",
				animation_reversed ? "(REVERSED)" : "(NORMAL)");
	ImGui::Text("Self Rotation: %s", model_spin ? "ON" : "OFF");

	ImGui::End();

	// Анимация движения по орбите (вокруг центра сцены)
	if (!animation_paused) {
		// Используем time для плавной анимации, независимой от FPS
		trajectory_angle = time * trajectory_speed * animation_direction;

		// Движение по кругу вокруг центра сцены (0,0,0)
		// model_position.x = cosf(trajectory_angle) * trajectory_radius;
		// model_position.z = sinf(trajectory_angle) * trajectory_radius;

		model_position.x = cosf(trajectory_angle) * trajectory_radius;
		model_position.y = 0.0f;  // Держим на уровне Y=0
		model_position.z = sinf(trajectory_angle) * trajectory_radius;
	}

	// Анимация вращения вокруг своей оси
	if (model_spin) {
		// Вращение вокруг своей оси Y с собственной скоростью
		model_rotation = time * rotation_speed;
	}

	// Нормализация углов (опционально, для избежания переполнения)
	trajectory_angle = fmodf(trajectory_angle, 2.0f * std::numbers::pi);
	model_rotation = fmodf(model_rotation, 2.0f * std::numbers::pi);
}

void render(VkCommandBuffer cmd, VkFramebuffer framebuffer) {
    vkResetCommandBuffer(cmd, 0);

    // Начало записи команд
    VkCommandBufferBeginInfo begin{ VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &begin);

    // Получаем размеры кадра в пикселях (HiDPI/Retina)
    ImVec2 fbScale = ImGui::GetIO().DisplayFramebufferScale;
    if (fbScale.x <= 0.0f) fbScale.x = 1.0f;
    if (fbScale.y <= 0.0f) fbScale.y = 1.0f;

    const uint32_t fbw = static_cast<uint32_t>(std::round(veekay::app.window_width  * fbScale.x));
    const uint32_t fbh = static_cast<uint32_t>(std::round(veekay::app.window_height * fbScale.y));

    // Начинаем рендер-проход с очисткой на весь framebuffer
    VkClearValue clear_color{ .color = {{0.1f, 0.1f, 0.1f, 1.0f}} };
    VkClearValue clear_depth{ .depthStencil = {1.0f, 0} };
    VkClearValue clears[] = { clear_color, clear_depth };

    VkRenderPassBeginInfo rp{ VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO };
    rp.renderPass  = veekay::app.vk_render_pass;
    rp.framebuffer = framebuffer;
    rp.renderArea.offset = {0, 0};
    rp.renderArea.extent = { fbw, fbh };
    rp.clearValueCount = 2;
    rp.pClearValues    = clears;
    vkCmdBeginRenderPass(cmd, &rp, VK_SUBPASS_CONTENTS_INLINE);

    // Пайплайн
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);

    // Динамический viewport/scissor по размерам framebuffer
    VkViewport vp{};
    vp.x = 0.0f; vp.y = 0.0f;
    vp.width  = static_cast<float>(fbw);
    vp.height = static_cast<float>(fbh);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    // vkCmdSetViewport(cmd, 0, 1, &vp);

    VkRect2D sc{};
    sc.offset = {0, 0};
    sc.extent = { fbw, fbh };
    // vkCmdSetScissor(cmd, 0, 1, &sc);

    // Буферы
    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &vertex_buffer.buffer, &offset);
    vkCmdBindIndexBuffer(cmd, index_buffer.buffer, 0, VK_INDEX_TYPE_UINT32);

	// Матрицы и цвет
	ShaderConstants constants{
		.projection = projection(
			camera_fov,                                              // Используем перспективу
			static_cast<float>(fbw) / static_cast<float>(fbh),      // aspect ratio
			camera_near_plane,
			camera_far_plane
		),
		.transform = multiply(
			multiply(rotation({0.0f, 1.0f, 0.0f}, model_rotation),      // Вращение вокруг Y
					 rotation({1.0f, 0.0f, 0.0f}, -0.3f)),              // Небольшой наклон
			translation({0.0f, 0.0f, -8.0f}) // Отодвигаем дальше
		),
		.color = model_color,
	};


    vkCmdPushConstants(cmd, pipeline_layout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(ShaderConstants), &constants);

    // Рисуем цилиндр
    vkCmdDrawIndexed(cmd, 8 * 6, 1, 0, 0, 0);

    // Завершаем
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
