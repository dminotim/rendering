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
        set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders/metal")
    elseif(WIN32)
        set(SHADER_DIR "${CMAKE_SOURCE_DIR}/shaders/glsl")
    endif()
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
    list(APPEND PLATFORM_SRC
            external/imgui/backends/imgui_impl_vulkan.cpp
    )
    # Example for a future DirectX 12 backend
    # file(GLOB_RECURSE DX12_IMPL_SRC
    #     "${CMAKE_SOURCE_DIR}/render_dx12/*.hpp"
    #     "${CMAKE_SOURCE_DIR}/render_dx12/*.cpp"
    # )
    # list(APPEND PLATFORM_SRC ${DX12_IMPL_SRC})
    #
    # # ImGui backend for DirectX 12
    # list(APPEND PLATFORM_SRC
    #     external/imgui/backends/imgui_impl_dx12.cpp
    # )
else()
    message(FATAL_ERROR "Unsupported platform! Only Apple and Windows are configured.")
endif()


# --- Main Application Target ---
set(APP_NAME "RenderingApp")

add_executable(${APP_NAME}
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
    target_link_libraries(${APP_NAME} PRIVATE
            glfw
            Vulkan::Vulkan
    )
endif()