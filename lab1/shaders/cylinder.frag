#version 450

// NOTE: Входные данные из вертексного шейдера (интерполированные)
layout (location = 0) in vec3 frag_color;

// NOTE: Выходной цвет пикселя
layout (location = 0) out vec4 final_color;

void main() {
    // NOTE: Используем интерполированный цвет от вершин
    final_color = vec4(frag_color, 1.0);
}