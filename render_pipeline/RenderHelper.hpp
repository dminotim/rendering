//
// Created by Artem Avdoshkin on 23.06.2025.
//

#ifndef RENDERING_RENDERHELPER_HPP
#define RENDERING_RENDERHELPER_HPP

#include <memory>
#include <filesystem>
// Include all necessary graphics interfaces
#include "Device.hpp"
#include "Surface.hpp"
#include "SwapChain.hpp"
#include "CommandQueue.hpp"
#include "Commandbuffer.hpp"
#include "ShaderFunction.hpp"
#include "Pipeline.hpp"
#include "RenderPassDescriptor.hpp"

// Forward declaration for GLFW
struct GLFWwindow;

namespace dmrender
{
/**
 * @namespace helper
 * @brief Provides factory functions for creating graphics objects and utility functions
 * for managing subsystems like ImGui.
 *
 * This namespace acts as a facade over the backend-specific implementations,
 * allowing platform-agnostic creation of resources.
 */
    namespace helper
    {
        // --- Factory Functions for Core Graphics Objects ---

        std::shared_ptr<Device> createDefaultDevice(const std::shared_ptr<Surface>& surface);

        std::shared_ptr<Surface> createSurface(GLFWwindow* window, ImageFormat imageFormat);

        std::shared_ptr<SwapChain> createSwapChain(
                const std::shared_ptr<Device>& device,
                const std::shared_ptr<CommandQueue>& commandQueue,
                const std::shared_ptr<Surface>& surface,
                size_t width,
                size_t height
        );

        std::shared_ptr<CommandQueue> createCommandQueue(const std::shared_ptr<Device>& device);

        std::shared_ptr<CommandBuffer> createCommandBuffer(const std::shared_ptr<CommandQueue>& commandQueue);

        std::shared_ptr<ShaderFunction> createShaderFunction(
                const std::shared_ptr<Device>& device,
                const std::filesystem::path& pathToShaderFile,
                const std::string& functionName
        );

        std::shared_ptr<Pipeline> createPipeline(
                const std::shared_ptr<Device>& device,
                const std::shared_ptr<ShaderFunction>& vertexFunction,
                const std::shared_ptr<ShaderFunction>& fragmentFunction,
                const RenderTargetFormat& targetFormat
        );

        std::shared_ptr<RenderPassDescriptor> createRenderPassDescriptor();

        // --- ImGui Helper Functions ---

        /**
         * @brief Initializes the ImGui backend.
         * @param device The graphics device.
         * @return True on success, false otherwise.
         */
        bool initImgui(const std::shared_ptr<Device>& device);

        /**
         * @brief Starts a new ImGui frame within the given render pass.
         * @param passDesc The current render pass descriptor.
         * @return True on success, false otherwise.
         */
        bool newFrameImgui(const std::shared_ptr<RenderPassDescriptor>& passDesc);

        /**
         * @brief Renders ImGui draw data into the given command buffer.
         * @param commandBuffer The command buffer to record rendering commands into.
         * @return True on success, false otherwise.
         */
        bool renderInternalImgui(const std::shared_ptr<CommandBuffer>& commandBuffer);

        /**
         * @brief Shuts down the ImGui backend.
         */
        bool shutdownImgui();

    } // namespace helper
} // namespace dmrender

#endif //RENDERING_RENDERHELPER_HPP
