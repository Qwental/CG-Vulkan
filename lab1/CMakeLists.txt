# lab1/CMakeLists.txt
cmake_minimum_required(VERSION 3.20)
project(lab1 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Vulkan REQUIRED)

add_executable(${PROJECT_NAME} main.cpp)

# связываем с библиотекой veekay и заголовками Vulkan
target_link_libraries(${PROJECT_NAME} veekay Vulkan::Headers)

# Компиляция шейдеров
find_program(GLSLC_FOUND glslc)
if(GLSLC_FOUND)
	set(_SHADER_BINARIES)
	function(compile_shader SHADER_FILE)
		set(SHADER_SOURCE ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_FILE})
		set(SHADER_BINARY ${CMAKE_CURRENT_SOURCE_DIR}/shaders/${SHADER_FILE}.spv)
		add_custom_command(
				OUTPUT ${SHADER_BINARY}
				COMMAND ${GLSLC_FOUND} ${SHADER_SOURCE} -o ${SHADER_BINARY}
				DEPENDS ${SHADER_SOURCE}
				COMMENT "Compiling ${SHADER_FILE}"
		)
		list(APPEND _SHADER_BINARIES ${SHADER_BINARY})
		set(_SHADER_BINARIES "${_SHADER_BINARIES}" PARENT_SCOPE)
	endfunction()

	compile_shader(shader.vert)
	compile_shader(shader.frag)

	add_custom_target(${PROJECT_NAME}_shaders DEPENDS ${_SHADER_BINARIES})
	add_dependencies(${PROJECT_NAME} ${PROJECT_NAME}_shaders)
endif()
