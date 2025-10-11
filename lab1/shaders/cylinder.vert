#version 450

// NOTE: Атрибуты вершин - должны соответствовать объявлению VkVertexInputAttribute
layout (location = 0) in vec3 v_position;
layout (location = 1) in vec3 v_color;

// NOTE: Выходные данные для фрагментного шейдера
layout (location = 0) out vec3 frag_color;

// NOTE: Push constants - должны соответствовать структуре ShaderConstants в C++
layout (push_constant, std430) uniform ShaderConstants {
    mat4 projection;
    mat4 transform;
};

void main() {
    // NOTE: Преобразуем позицию вершины
    vec4 world_position = transform * vec4(v_position, 1.0);
    vec4 clip_position = projection * world_position;

    // NOTE: Передаем цвет в фрагментный шейдер для интерполяции
    frag_color = v_color;

    // NOTE: Выходная позиция в clip space
    gl_Position = clip_position;
}