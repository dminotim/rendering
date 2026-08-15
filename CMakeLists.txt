# --- Project Definition ---
# Specifies the minimum required version of CMake and defines the project.
cmake_minimum_required(VERSION 3.20) # Lowered for better compatibility
if(APPLE)
    project(RenderingProject LANGUAGES C CXX OBJC OBJCXX)
else()
    project(RenderingProject LANGUAGES C CXX)
endif()

# Set the C++ standard for the entire project.
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF) # Good practice to disable compiler-specific extensions

# --- Resource Management: Define shader path ---
# This creates a compile-time definition for the shader directory.
# We check if a custom path is provided, otherwise use a default relative path.
if(NOT DEFINED SHADER_DIR)
    if(APPLE)
        # Metal compiles its shaders at runtime, so the sources are used in place.
        set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders/metal")
    elseif(WIN32)
        # Vulkan consumes pre-compiled SPIR-V, so SHADER_DIR points at the build output
        # rather than at the GLSL sources in shaders/glsl.
        set(SHADER_DIR "${CMAKE_BINARY_DIR}/shaders")
    endif()
endif()
option(ENABLE_VULKAN_DEBUG "Enable feature FOO" OFF)

if(ENABLE_VULKAN_DEBUG)
    add_compile_definitions(ENABLE_VALIDATION_LAYER=1)
else()
    add_compile_definitions(ENABLE_VALIDATION_LAYER=0)
endif()
# Add the definition to all targets in the project.
add_compile_definitions(SHADER_DIR="${SHADER_DIR}")
message(STATUS "Shaders directory set to: ${SHADER_DIR}")

# --- External Dependencies ---
# Add submodules/external libraries.
find_package(Vulkan REQUIRED)
add_subdirectory(external/glfw)

# --- Source File Collection (Automated) ---
# This is much cleaner and more maintainable than listing every file.

# 1. Gather all abstract interface files from the render_pipeline directory.
file(GLOB_RECURSE PIPELINE_INTERFACE_SRC
        "${CMAKE_SOURCE_DIR}/render_pipeline/*.hpp"
        "${CMAKE_SOURCE_DIR}/render_pipeline/*.cpp"
)

# 2. Gather platform-specific implementation files.
# We'll add them to the main source list based on the target OS.
set(PLATFORM_SRC "")
if(APPLE)
    message(STATUS "Configuring for Apple (macOS/iOS)")
    # Gather all Metal implementation files.
    file(GLOB_RECURSE METAL_IMPL_SRC
            "${CMAKE_SOURCE_DIR}/render_metal/*.hpp"
            "${CMAKE_SOURCE_DIR}/render_metal/*.mm"
    )
    list(APPEND PLATFORM_SRC ${METAL_IMPL_SRC})

    # ImGui backend for Metal
    list(APPEND PLATFORM_SRC
            external/imgui/backends/imgui_impl_metal.mm
    )
elseif(WIN32)
    message(STATUS "Configuring for Windows")
     file(GLOB_RECURSE VULKAN_IMPL_SRC
            "${CMAKE_SOURCE_DIR}/render_vulkan/*.hpp"
            "${CMAKE_SOURCE_DIR}/render_vulkan/*.cpp"
    )
    list(APPEND PLATFORM_SRC ${VULKAN_IMPL_SRC})
    list(APPEND PLATFORM_SRC
            external/imgui/backends/imgui_impl_vulkan.cpp
    )
else()
    message(FATAL_ERROR "Unsupported platform! Only Apple and Windows are configured.")
endif()


# --- Main Application Target ---
set(APP_NAME "RenderingApp")

set(ALL_SOURCE_FILES
        # Core application files
        main.cpp
        FastRenderer.hpp
        FastRenderer.cpp

        # ImGui core and common backend files
        external/imgui/imgui.cpp
        external/imgui/imgui_draw.cpp
        external/imgui/imgui_tables.cpp
        external/imgui/imgui_widgets.cpp
        external/imgui/backends/imgui_impl_glfw.cpp

        # Abstract interfaces
        ${PIPELINE_INTERFACE_SRC}

        # Platform-specific implementation
        ${PLATFORM_SRC}
)

add_executable(${APP_NAME} ${ALL_SOURCE_FILES})

# --- Shader Compilation (Vulkan only) ---
# Metal compiles PlaneShader.metal at runtime and picks the entry point by name. Vulkan has no
# runtime GLSL compiler, so each entry point is compiled to its own SPIR-V module here. The file
# names encode the same logical function names the Metal code uses:
#
#   plane_vertex_shader   -> ${SHADER_DIR}/PlaneShader.plane_vertex_shader.spv
#   plane_fragment_shader -> ${SHADER_DIR}/PlaneShader.plane_fragment_shader.spv
#
# which is exactly what VulkanShaderFunction looks for, so application code can keep passing an
# extensionless path plus a function name on both platforms.
if(WIN32)
    find_program(GLSLC_EXECUTABLE
            NAMES glslc glslc.exe
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
            DOC "glslc shader compiler shipped with the Vulkan SDK"
    )
    if(NOT GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK or set VULKAN_SDK.")
    endif()
    message(STATUS "Using shader compiler: ${GLSLC_EXECUTABLE}")

    # source file : logical function name : name of the .metal file it corresponds to
    #
    # The last field exists because several GLSL sources map onto entry points of a single Metal
    # file — PlaneShader.metal holds both plane_fragment_shader and plane_fragment_shader_mrt —
    # and the artifact must be named after the file the application asks for, not after the GLSL
    # source it happened to be compiled from.
    set(GLSL_SHADERS
            "PlaneShader.vert:plane_vertex_shader:PlaneShader"
            "PlaneShader.frag:plane_fragment_shader:PlaneShader"
            "PlaneShaderMRT.frag:plane_fragment_shader_mrt:PlaneShader"
            "Composite.vert:composite_vertex_shader:Composite"
            "Composite.frag:composite_fragment_shader:Composite"
            "CompositeMRT.frag:composite_fragment_shader_mrt:Composite"
            "Overlay.vert:overlay_vertex_shader:Overlay"
            "Overlay.frag:overlay_fragment_shader:Overlay"
    )

    set(SPIRV_OUTPUTS "")
    foreach(_shader_entry ${GLSL_SHADERS})
        string(REPLACE ":" ";" _parts "${_shader_entry}")
        list(GET _parts 0 _shader_file)
        list(GET _parts 1 _function_name)
        list(GET _parts 2 _shader_stem)

        set(_shader_src "${CMAKE_SOURCE_DIR}/shaders/glsl/${_shader_file}")
        set(_spirv_out "${SHADER_DIR}/${_shader_stem}.${_function_name}.spv")

        add_custom_command(
                OUTPUT "${_spirv_out}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_DIR}"
                COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2 -o "${_spirv_out}" "${_shader_src}"
                DEPENDS "${_shader_src}"
                COMMENT "Compiling ${_shader_file} -> ${_shader_stem}.${_function_name}.spv"
                VERBATIM
        )
        list(APPEND SPIRV_OUTPUTS "${_spirv_out}")
    endforeach()

    add_custom_target(Shaders DEPENDS ${SPIRV_OUTPUTS})
    add_dependencies(${APP_NAME} Shaders)

    # Keep the GLSL sources visible in the IDE next to the rest of the project.
    set(GLSL_SOURCE_FILES
            shaders/glsl/PlaneShader.vert
            shaders/glsl/PlaneShader.frag
            shaders/glsl/PlaneShaderMRT.frag
            shaders/glsl/Composite.vert
            shaders/glsl/Composite.frag
            shaders/glsl/CompositeMRT.frag
            shaders/glsl/Overlay.vert
            shaders/glsl/Overlay.frag
    )
    target_sources(${APP_NAME} PRIVATE ${GLSL_SOURCE_FILES})
    set_source_files_properties(${GLSL_SOURCE_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

# --- Target Configuration: Include Directories ---
target_include_directories(${APP_NAME} PRIVATE
        # Public headers for external libraries
        external/glfw/include
        external/imgui
        external/imgui/backends

        # Project-specific headers
        . # Include the root project directory for FastRenderer.hpp etc.
        render_pipeline
)

# --- Platform-Specific Configuration ---
if(APPLE)
    # For Metal backend, we need to include its header directory.
    target_include_directories(${APP_NAME} PRIVATE render_metal)

    # Link against required Apple frameworks for Metal and windowing.
    target_link_libraries(${APP_NAME} PRIVATE
            glfw
            "-framework Cocoa"
            "-framework QuartzCore"
            "-framework Metal"
            "-framework Foundation" # Often needed for Objective-C objects
    )
elseif(WIN32)
    target_include_directories(${APP_NAME} PRIVATE render_vulkan)
    target_link_libraries(${APP_NAME} PRIVATE
            glfw
            Vulkan::Vulkan
    )
if(MSVC)
    foreach(_source ${ALL_SOURCE_FILES})
	    if (IS_ABSOLUTE "${_source}")
		    file(RELATIVE_PATH _source_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_source}")
	    else()
		    set(_source_rel "${_source}")
	    endif()
	    get_filename_component(_source_path "${_source_rel}" PATH)
	    string(REPLACE "/" "\\" _source_path_msvc "${_source_path}")
	    source_group("${_source_path_msvc}" FILES "${_source}")
    endforeach()
    target_compile_options(${APP_NAME} PRIVATE /bigobj /WX /W0)
endif(MSVC)

endif()