//
// Created by Artem Avdoshkin on 23.06.2025.
//

#include "RenderHelper.hpp"
#include <fstream>
#include <sstream>

// --- Platform specific implementations ---
#if defined(__APPLE__)
    #include "MetalDevice.hpp"
    #include "MetalOutSurface.hpp"
    #include "MetalSwapChain.hpp"
    #include "MetalShaderFunction.hpp"
    #include "MetalPipeline.hpp"
    #include "MetalRenderPassDescriptor.hpp"
    #include "MetalCommandbuffer.hpp"
    #include "MetalCommandQueues.hpp"
    #include "MetalUtils.hpp"
#else
    #include <render_vulkan/VulkanCommandQueue.hpp>
    #include <render_vulkan/VulkanDevice.hpp>
    #include <render_vulkan/VulkanPipeline.hpp>
    #include <render_vulkan/VulkanRenderPassDescriptor.hpp>
    #include <render_vulkan/VulkanShaderFunction.hpp>
    #include <render_vulkan/VulkanSurface.hpp>
    #include <render_vulkan/VulkanSwapChain.hpp>
    #include <render_vulkan/VulkanUtils.hpp>
#endif

namespace dmrender
{
    namespace helper
    {
        std::shared_ptr<Device> createDefaultDevice(const std::shared_ptr<Surface>& surface)
        {
            #if defined(__APPLE__)
                return MetalDevice::createDefaultDevice(surface);
            #else
                return VulkanDevice::createDefaultDevice(surface);
            #endif
        }

        std::shared_ptr<Surface> createSurface(GLFWwindow *window, ImageFormat imageFormat)
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalOutSurface>(window, imageFormat);
            #else
                return std::make_shared<VulkanSurface>(window, imageFormat);
            #endif
        }

        std::shared_ptr<SwapChain> createSwapChain(
                const std::shared_ptr<Device>& device,
                const std::shared_ptr<CommandQueue>& cmdLists,
                const std::shared_ptr<Surface>& outSurf,
                size_t width,
                size_t height)
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalSwapChain>(device, cmdLists, outSurf, width, height);
            #else
                return std::make_shared<VulkanSwapChain>(device, cmdLists, outSurf,
                                                         static_cast<uint32_t>(width),
                                                         static_cast<uint32_t>(height));
            #endif
        }

        /**
         * @brief Loads one shader entry point, resolving the backend's own file naming.
         *
         * Callers pass an *extensionless* path plus the logical function name, so the same line
         * of application code works on both backends:
         *
         *   Metal : reads <path>.metal and compiles it, then picks out `functionName`.
         *   Vulkan: loads the pre-compiled <path>.<functionName>.spv produced by the build.
         */
        std::shared_ptr<ShaderFunction> createShaderFunction(const std::shared_ptr<Device>& device,
                                                             const std::filesystem::path& pathToShaderFile,
                                                             const std::string& functionName)
        {
            #if defined(__APPLE__)
                std::filesystem::path sourcePath = pathToShaderFile;
                if (!sourcePath.has_extension()) {
                    sourcePath.replace_extension(".metal");
                }

                std::ifstream shaderFile(sourcePath);
                if (!shaderFile.is_open()) {
                    return nullptr;
                }
                std::stringstream shaderStream;
                shaderStream << shaderFile.rdbuf();
                std::string shaderSource = shaderStream.str();

                return std::make_shared<MetalShaderFunction>(device, shaderSource, functionName);
            #else
                return std::make_shared<VulkanShaderFunction>(device, pathToShaderFile, functionName);
            #endif
        }

        std::shared_ptr<Pipeline> createPipeline(const std::shared_ptr<Device>& device,
                                                 const std::shared_ptr<ShaderFunction>& vertexFunction,
                                                 const std::shared_ptr<ShaderFunction>& fragmentFunction,
                                                 const RenderTargetFormat& targetFormat)
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalPipeline>(device, vertexFunction, fragmentFunction, targetFormat);
            #else
                return std::make_shared<VulkanPipeline>(device, vertexFunction, fragmentFunction, targetFormat);
            #endif
        }

        std::shared_ptr<RenderPassDescriptor> createRenderPassDescriptor()
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalRenderPassDescriptor>();
            #else
                return std::make_shared<VulkanRenderPassDescriptor>();
            #endif
        }

        std::shared_ptr<CommandBuffer> createCommandBuffer(const std::shared_ptr<CommandQueue>& cmdQueue)
        {
            // Both backends produce their command buffer the same way, so no branch is needed.
            return cmdQueue->getCommandBuffer();
        }

        std::shared_ptr<CommandQueue> createCommandQueue(const std::shared_ptr<Device>& device)
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalCommandQueues>(device);
            #else
                return std::make_shared<VulkanCommandQueues>(device);
            #endif
        }

        // --- Imgui helpers ---

        bool initImgui(const std::shared_ptr<SwapChain>& swapChain)
        {
            #if defined(__APPLE__)
                return InitImguiMetal(swapChain);
            #else
                return InitImguiVulkan(swapChain);
            #endif
        }

        bool newFrameImgui(const std::shared_ptr<RenderPassDescriptor>& passDesc)
        {
            #if defined(__APPLE__)
                return NewFrameImguiMetal(passDesc);
            #else
                return NewFrameImguiVulkan(passDesc);
            #endif
        }

        bool renderInternalImgui(const std::shared_ptr<CommandBuffer>& cmdBuffer)
        {
            #if defined(__APPLE__)
                return RenderInternalImguiMetal(cmdBuffer);
            #else
                return RenderInternalImguiVulkan(cmdBuffer);
            #endif
        }

        bool shutdownImgui()
        {
            #if defined(__APPLE__)
                return ShutdownImguiMetal();
            #else
                return ShutdownImguiVulkan();
            #endif
        }

    } // namespace helper
} // namespace dmrender
