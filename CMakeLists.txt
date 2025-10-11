# CMakeLists.txt (корень)
cmake_minimum_required(VERSION 3.20)
include(FetchContent)

project(veekay LANGUAGES C CXX)

# 1) Сначала создаём цель veekay
add_library(veekay source/veekay.cpp)

# 2) Затем задаём include-директории для СУЩЕСТВУЮЩЕЙ цели
target_include_directories(veekay PUBLIC
		$<BUILD_INTERFACE:${veekay_SOURCE_DIR}/include>
		$<INSTALL_INTERFACE:include>
)

# 3) Компилятор/линковщик по платформам
if(APPLE)
	target_compile_options(veekay PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
	set_target_properties(veekay PROPERTIES
			BUILD_RPATH "/opt/homebrew/lib;/opt/homebrew/opt/vulkan-loader/lib;$ENV{VULKAN_SDK}/lib"
			INSTALL_RPATH "/opt/homebrew/lib;/opt/homebrew/opt/vulkan-loader/lib;$ENV{VULKAN_SDK}/lib"
			BUILD_WITH_INSTALL_RPATH FALSE)
elseif(MSVC)
	target_compile_options(veekay PRIVATE /W4)
	target_link_options(veekay PRIVATE /subsystem:windows /entry:mainCRTStartup)
elseif(LINUX)
	target_compile_options(veekay PRIVATE -Wall -Wextra -Wno-missing-field-initializers)
else()
	message(FATAL_ERROR "${CMAKE_SYSTEM_NAME} is not supported")
endif()

set_target_properties(veekay PROPERTIES CXX_STANDARD_REQUIRED TRUE CXX_STANDARD 20)

# 4) Зависимости
FetchContent_Declare(glfw GIT_REPOSITORY https://github.com/glfw/glfw.git GIT_TAG 3.4)
FetchContent_Declare(vk-bootstrap GIT_REPOSITORY https://github.com/charles-lunarg/vk-bootstrap GIT_TAG v1.4.321)
FetchContent_Declare(imgui GIT_REPOSITORY https://github.com/ocornut/imgui GIT_TAG v1.92.3)

find_package(Vulkan REQUIRED)
set(GLFW_LIBRARY_TYPE STATIC)
set(GLFW_BUILD_EXAMPLES OFF)
set(GLFW_BUILD_TESTS OFF)
set(GLFW_BUILD_DOCS OFF)
FetchContent_MakeAvailable(glfw vk-bootstrap imgui)

# 5) Линкуем зависимости к veekay
target_link_libraries(veekay PRIVATE
		glfw
		Vulkan::Vulkan
		vk-bootstrap::vk-bootstrap
)

# 6) Подключаем ImGui к veekay
target_include_directories(veekay PUBLIC ${imgui_SOURCE_DIR} ${imgui_SOURCE_DIR}/backends)
target_sources(veekay PRIVATE
		${imgui_SOURCE_DIR}/imgui.cpp
		${imgui_SOURCE_DIR}/imgui_draw.cpp
		${imgui_SOURCE_DIR}/imgui_tables.cpp
		${imgui_SOURCE_DIR}/imgui_widgets.cpp
		${imgui_SOURCE_DIR}/backends/imgui_impl_glfw.cpp
		${imgui_SOURCE_DIR}/backends/imgui_impl_vulkan.cpp
)

# 7) Подпроекты (сначала библиотека, потом исполняемые)
add_subdirectory(testbed)
add_subdirectory(lab1)
