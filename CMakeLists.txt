# --- Project Definition ---
cmake_minimum_required(VERSION 3.20)

project(RenderingProject LANGUAGES C CXX)

# Objective-C++ is enabled after project() rather than listed in it: APPLE is only defined once
# project() has run, so testing it beforehand silently takes the wrong branch.
if(APPLE)
    enable_language(OBJCXX)
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --- The graphics wrapper -----------------------------------------------------------------------
#
# dmrender used to live in this repository under render_pipeline/, render_vulkan/ and
# render_metal/. It is now a library with its own repository, and this project is one of its
# consumers: nothing below knows which graphics API is underneath.
#
# Found two ways. An installed copy wins if there is one; otherwise a source checkout is added
# directly, which is what a side-by-side working copy wants. Source builds also give this project
# the `glfw` and `imgui` targets, which it uses for the window and its own interface.
# The repository is named dmwrapper while the library and its CMake target are named dmrender, so
# a plain `git clone` lands in ../dmwrapper and an explicitly named one in ../dmrender. Both are
# accepted: making the default depend on how the dependency happened to be cloned is a bad trade
# for one line of loop.
set(_dmrender_default "${CMAKE_CURRENT_SOURCE_DIR}/../dmwrapper")
foreach(_candidate dmwrapper dmrender)
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../${_candidate}/CMakeLists.txt")
        set(_dmrender_default "${CMAKE_CURRENT_SOURCE_DIR}/../${_candidate}")
        break()
    endif()
endforeach()
set(DMRENDER_DIR "${_dmrender_default}"
        CACHE PATH "Source checkout of the dmrender wrapper (repository: dmwrapper)")

find_package(dmrender QUIET CONFIG)
if(dmrender_FOUND)
    message(STATUS "Using installed dmrender ${dmrender_VERSION}")
else()
    if(NOT EXISTS "${DMRENDER_DIR}/CMakeLists.txt")
        message(FATAL_ERROR
                "dmrender not found (looked in ${DMRENDER_DIR}).\n"
                "Clone it next to this repository:\n"
                "    git clone --recurse-submodules https://github.com/dminotim/dmwrapper.git\n"
                "or set DMRENDER_DIR to an existing checkout, or install it and let\n"
                "find_package(dmrender) pick it up.")
    endif()
    message(STATUS "Using dmrender from source: ${DMRENDER_DIR}")
    add_subdirectory("${DMRENDER_DIR}" "${CMAKE_BINARY_DIR}/dmrender")
endif()

# --- Resource Management: Define shader path ----------------------------------------------------
#
# The renderer's own shaders, not the wrapper's. Metal compiles its sources at runtime so they are
# used in place; Vulkan consumes pre-compiled SPIR-V, so the path points at the build output.
if(NOT DEFINED SHADER_DIR)
    if(APPLE)
        set(SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/shaders/metal")
    else()
        set(SHADER_DIR "${CMAKE_BINARY_DIR}/shaders")
    endif()
endif()
message(STATUS "Shaders directory set to: ${SHADER_DIR}")

# --- Main Application Target --------------------------------------------------------------------
set(APP_NAME "RenderingApp")

set(ALL_SOURCE_FILES
        # Core application files
        main.cpp
        FastRenderer.hpp
        FastRenderer.cpp

        # Model loading (application-level: the wrapper knows nothing about file formats)
        mesh/Mesh.hpp
        mesh/Mesh.cpp
        mesh/FbxLoader.cpp
        mesh/SceneKit.cpp
        mesh/SceneCache.cpp

        # Texture loading and sharing
        texture/TextureCache.hpp
        texture/TextureCache.cpp
)

add_executable(${APP_NAME} ${ALL_SOURCE_FILES})

# SHADER_DIR is this target's business alone; the wrapper takes shader paths as arguments.
target_compile_definitions(${APP_NAME} PRIVATE SHADER_DIR="${SHADER_DIR}")

# tinyobjloader embeds a copy of fast_float whose C++20 constexpr paths MSVC rejects: the
# SIMD digit-parsing helpers are marked constexpr but call intrinsics, and MSVC enforces that
# strictly. Compiling just this translation unit as C++17 turns those paths off — the feature
# macros fast_float tests for are simply absent — without patching vendored code or lowering
# the standard for the whole project.
if(MSVC)
    set_source_files_properties(mesh/Mesh.cpp PROPERTIES COMPILE_OPTIONS "/std:c++17")
endif()

# --- Shader Compilation (Vulkan only) -----------------------------------------------------------
# Metal compiles .metal sources at runtime and picks the entry point by name. Vulkan has no runtime
# GLSL compiler, so each entry point is compiled to its own SPIR-V module here. The file names
# encode the same logical function names the Metal code uses:
#
#   mesh_vertex_shader -> ${SHADER_DIR}/Mesh.mesh_vertex_shader.spv
#
# which is exactly what the wrapper's Vulkan shader loader looks for, so application code can keep
# passing an extensionless path plus a function name on both platforms.
if(NOT APPLE)
    find_program(GLSLC_EXECUTABLE
            NAMES glslc glslc.exe
            HINTS "$ENV{VULKAN_SDK}/Bin" "$ENV{VULKAN_SDK}/bin"
            DOC "glslc shader compiler shipped with the Vulkan SDK"
    )
    if(NOT GLSLC_EXECUTABLE)
        message(FATAL_ERROR "glslc was not found. Install the Vulkan SDK or set VULKAN_SDK.")
    endif()
    message(STATUS "Using shader compiler: ${GLSLC_EXECUTABLE}")

    # source file | logical function name | stem of the .metal file | extra glslc flags
    #
    # The third field exists because several GLSL sources map onto entry points of a single Metal
    # file, and the artifact must be named after the file the application asks for rather than
    # after the GLSL source it happened to be compiled from.
    #
    # Mesh.frag is compiled twice from one source: the masked variant differs only by an alpha
    # test, and keeping two near-identical files in sync by hand is exactly the kind of thing
    # that quietly diverges.
    set(GLSL_SHADERS
            "Mesh.vert|mesh_vertex_shader|Mesh|"
            "Mesh.frag|mesh_fragment_shader|Mesh|"
            "Mesh.frag|mesh_fragment_masked|Mesh|-DALPHA_TEST=1"
            "MeshShadow.vert|shadow_vertex_shader|MeshShadow|"
            "MeshShadow.frag|shadow_fragment_shader|MeshShadow|"
            "MeshShadow.frag|shadow_fragment_masked|MeshShadow|-DALPHA_TEST=1"
    )

    set(SPIRV_OUTPUTS "")
    foreach(_shader_entry ${GLSL_SHADERS})
        string(REPLACE "|" ";" _parts "${_shader_entry}")
        list(GET _parts 0 _shader_file)
        list(GET _parts 1 _function_name)
        list(GET _parts 2 _shader_stem)
        list(GET _parts 3 _extra_flags)

        set(_shader_src "${CMAKE_CURRENT_SOURCE_DIR}/shaders/glsl/${_shader_file}")
        set(_spirv_out "${SHADER_DIR}/${_shader_stem}.${_function_name}.spv")

        separate_arguments(_flag_list UNIX_COMMAND "${_extra_flags}")

        add_custom_command(
                OUTPUT "${_spirv_out}"
                COMMAND ${CMAKE_COMMAND} -E make_directory "${SHADER_DIR}"
                COMMAND "${GLSLC_EXECUTABLE}" --target-env=vulkan1.2 ${_flag_list} -o "${_spirv_out}" "${_shader_src}"
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
            shaders/glsl/Mesh.vert
            shaders/glsl/Mesh.frag
            shaders/glsl/MeshShadow.vert
            shaders/glsl/MeshShadow.frag
    )
    target_sources(${APP_NAME} PRIVATE ${GLSL_SOURCE_FILES})
    set_source_files_properties(${GLSL_SOURCE_FILES} PROPERTIES HEADER_FILE_ONLY TRUE)
endif()

# --- Include Directories ------------------------------------------------------------------------
#
# Only this project's own headers and its header-only loaders. The wrapper's public headers arrive
# through the dmrender target, and GLFW's and ImGui's through theirs.
target_include_directories(${APP_NAME} PRIVATE
        # Header-only model loaders (all MIT licensed)
        external/tinyobjloader     # OBJ + MTL, including texture references
        external/happly            # PLY, binary and ASCII
        external/microstl          # STL, binary and ASCII
        external/stb               # stb_image: PNG/JPG/TGA decoding (public domain)

        .                          # FastRenderer.hpp, mesh/, texture/
)

# --- Linking ------------------------------------------------------------------------------------
#
# glfw and imgui are named explicitly rather than inherited: this project uses both directly — it
# creates the window and draws its own interface — so it declares them itself rather than relying
# on what the wrapper happens to expose.
target_link_libraries(${APP_NAME} PRIVATE
        dmrender::dmrender
        glfw
        imgui
)

if(MSVC)
    foreach(_source ${ALL_SOURCE_FILES})
        if(IS_ABSOLUTE "${_source}")
            file(RELATIVE_PATH _source_rel "${CMAKE_CURRENT_SOURCE_DIR}" "${_source}")
        else()
            set(_source_rel "${_source}")
        endif()
        get_filename_component(_source_path "${_source_rel}" PATH)
        string(REPLACE "/" "\\" _source_path_msvc "${_source_path}")
        source_group("${_source_path_msvc}" FILES "${_source}")
    endforeach()
    target_compile_options(${APP_NAME} PRIVATE /bigobj /WX /W0)
endif()
