//
// Created by Artem Avdoshkin on 23.06.2025.
//
//
// RenderHelper.mm
//
// Created by Artem Avdoshkin on 23.06.2025.
//

#include "RenderHelper.hpp"
#include <fstream>
#include <sstream>

// --- Платформо-зависимые инклюды ---
#if defined(__APPLE__)
// Включаем реализации для Metal
#include "MetalDevice.hpp"
#include "MetalOutSurface.hpp"
#include "MetalSwapChain.hpp"
#include "MetalShaderFunction.hpp"
#include "MetalPipeline.hpp"
#include "MetalRenderPassDescriptor.hpp"
#include "MetalCommandbuffer.hpp"
#include "MetalCommandQueues.hpp"
#include "MetalUtils.hpp"

#endif

namespace dmrender
{
    namespace helper
    {
        std::shared_ptr<Device> createDefaultDevice()
        {
            #if defined(__APPLE__)
                return MetalDevice::createDefaultDevice();
            #else
                static_assert(false, "CreateDefaultDevice is not implemented for this platform.");
                return nullptr;
            #endif
        }

        std::shared_ptr<Surface> createSurface(GLFWwindow *window, const std::shared_ptr<Device>& device, ImageFormat imageFormat)
        {
            #if defined(__APPLE__)
                // .get() нужен, чтобы передать сырой указатель в конструктор
                return std::make_shared<MetalOutSurface>(window, device, imageFormat);
            #else
                static_assert(false, "CreateOutSurface is not implemented for this platform.");
                return nullptr;
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
                static_assert(false, "CreateSwapChain is not implemented for this platform.");
                return nullptr;
            #endif
        }

        std::shared_ptr<ShaderFunction> createShaderFunction(const std::shared_ptr<Device>& device,
                                                             const std::filesystem::path& pathToShaderFile, const std::string& functionName)
        {
            #if defined(__APPLE__)
                std::ifstream shaderFile(pathToShaderFile);
                if (!shaderFile.is_open()) {
                    // В реальном проекте здесь нужна обработка ошибок получше (логирование, исключение)
                    return nullptr;
                }
                std::stringstream shaderStream;
                shaderStream << shaderFile.rdbuf();
                std::string shaderSource = shaderStream.str();

                return std::make_shared<MetalShaderFunction>(device, shaderSource, functionName);
            #else
                static_assert(false, "CreateShaderFunction is not implemented for this platform.");
                return nullptr;
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
                static_assert(false, "CreatePipeline is not implemented for this platform.");
                return nullptr;
            #endif
        }

        std::shared_ptr<RenderPassDescriptor> createRenderPassDescriptor()
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalRenderPassDescriptor>();
            #else
                static_assert(false, "CreateRenderPassDescriptor is not implemented for this platform.");
                return nullptr;
            #endif
        }

        std::shared_ptr<CommandBuffer> createCommandBuffer(const std::shared_ptr<CommandQueue>& cmdQueue)
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalCommandBuffer>(cmdQueue);
            #else
                static_assert(false, "CreateCommandBuffer is not implemented for this platform.");
                return nullptr;
            #endif
        }

        std::shared_ptr<CommandQueue> createCommandQueue(const std::shared_ptr<Device>& device)
        {
            #if defined(__APPLE__)
                return std::make_shared<MetalCommandQueues>(device);
            #else
                static_assert(false, "CreateCommnadQueues is not implemented for this platform.");
                return nullptr;
            #endif
        }

        // --- Imgui helpers ---

        bool initImgui(const std::shared_ptr<Device>& device)
        {
            #if defined(__APPLE__)
                return InitImguiMetal(device);
            #else
                static_assert(false, "InitImgui is not implemented for this platform.");
                return false;
            #endif
        }

        bool newFrameImgui(const std::shared_ptr<RenderPassDescriptor>& passDesc)
        {
            #if defined(__APPLE__)
                return NewFrameImguiMetal(passDesc);
            #else
                static_assert(false, "NewFrameImgui is not implemented for this platform.");
                return false;
            #endif
        }

        bool renderInternalImgui(const std::shared_ptr<CommandBuffer>& cmdBuffer)
        {
            #if defined(__APPLE__)
                return RenderInternalImguiMetal(cmdBuffer);
            #else
                static_assert(false, "RenderInternalImgui is not implemented for this platform.");
                return false;
            #endif
        }

        bool shutdownImgui()
        {
            #if defined(__APPLE__)
                return ShutdownImguiMetal();
            #else
                static_assert(false, "ShutdownImgui is not implemented for this platform.");
                return false;
            #endif
        }

    } // namespace helper
} // namespace dmrender
