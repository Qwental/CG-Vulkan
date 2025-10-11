#pragma once
#include <vulkan/vulkan.h>

namespace lab1 {

    using InitFunc     = void(*)();
    using ShutdownFunc = void(*)();
    using UpdateFunc   = void(*)(double time);
    using RenderFunc   = void(*)(VkCommandBuffer, VkFramebuffer);

    // Тонкий адаптер, который вызывает движковый цикл veekay::run
    int run(InitFunc init, ShutdownFunc shutdown, UpdateFunc update, RenderFunc render);

} // namespace lab1
