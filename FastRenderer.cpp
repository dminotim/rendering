// ───────────────────────────────────────────────
// FastRenderer.cpp
// ───────────────────────────────────────────────
#include "FastRenderer.hpp"
#include <GLFW/glfw3.h>
#include <memory>

#include "imgui.h"
#include "backends/imgui_impl_glfw.h"

#include "Device.hpp"
#include "Surface.hpp"
#include "SwapChain.hpp"
#include "RenderPassDescriptor.hpp"
#include "GBuffer.hpp"
#include "GImage.hpp"
#include "GSampler.hpp"
#include "Commandbuffer.hpp"
#include "CommandQueue.hpp"
#include "ShaderFunction.hpp"
#include "Pipeline.hpp"

#include "RenderHelper.hpp"

namespace dmrender
{
    struct Vertex {
        float position[2];
    };

    struct Uniforms {
        float viewportSize[2];
        float scale;
        float pan[2];
    };

    struct CompositeUniforms {
        float viewportSize[2];
        float splitX;
    };

    /// Pixel format of the offscreen render targets. Linear, like the swapchain, so a value
    /// written by the plane pass survives the round trip through the composite pass untouched.
    constexpr ImageFormat kTargetFormat = ImageFormat::BGRA8_UNORM;

    void FramebufferSizeCallback(GLFWwindow* window, int width, int height) {
        auto* ctx = static_cast<std::shared_ptr<SwapChain>*>(glfwGetWindowUserPointer(window));
        if (*(ctx)) {
            (*ctx)->recreate(width, height);
        }
    }

    void run_loop()
    {
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window = glfwCreateWindow(1280, 720, "Grid Renderer", nullptr, nullptr);
        if (!window) { glfwTerminate(); return; }
        std::shared_ptr<Surface> surface = helper::createSurface(window, ImageFormat::BGRA8_UNORM);
        std::shared_ptr<Device> device = helper::createDefaultDevice(surface);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForOther(window, true);

        std::shared_ptr<CommandQueue> cmdLists = helper::createCommandQueue(device);

        int fbWidth, fbHeight;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);

        std::shared_ptr<SwapChain> swapChain = helper::createSwapChain(device, cmdLists, surface, fbWidth, fbHeight);
        glfwSetWindowUserPointer(window, &swapChain);
        glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);

        helper::initImgui(swapChain); // после свопчейна: Vulkan-бэкенду нужны его render pass и image count

        // --- Шейдеры. Путь без расширения: каждый бэкенд достраивает своё. ---
        const std::filesystem::path planePath = std::filesystem::path(SHADER_DIR) / "PlaneShader";
        const std::filesystem::path compositePath = std::filesystem::path(SHADER_DIR) / "Composite";

        std::shared_ptr<ShaderFunction> planeVertexFunction =
            helper::createShaderFunction(device, planePath, "plane_vertex_shader");
        std::shared_ptr<ShaderFunction> planeFragmentFunction =
            helper::createShaderFunction(device, planePath, "plane_fragment_shader");
        std::shared_ptr<ShaderFunction> planeFragmentMRTFunction =
            helper::createShaderFunction(device, planePath, "plane_fragment_shader_mrt");

        std::shared_ptr<ShaderFunction> compositeVertexFunction =
            helper::createShaderFunction(device, compositePath, "composite_vertex_shader");
        std::shared_ptr<ShaderFunction> compositeFragmentFunction =
            helper::createShaderFunction(device, compositePath, "composite_fragment_shader");
        std::shared_ptr<ShaderFunction> compositeFragmentMRTFunction =
            helper::createShaderFunction(device, compositePath, "composite_fragment_shader_mrt");

        // --- Пайплайны. Количество цветовых форматов «зашивается» в пайплайн, поэтому
        //     одноцелевой и MRT-варианты — это два разных объекта, созданных заранее. ---
        std::shared_ptr<Pipeline> planePipeline = helper::createPipeline(
            device, planeVertexFunction, planeFragmentFunction,
            RenderTargetFormat::singleTarget(kTargetFormat));

        std::shared_ptr<Pipeline> planePipelineMRT = helper::createPipeline(
            device, planeVertexFunction, planeFragmentMRTFunction,
            RenderTargetFormat::multiTarget({ kTargetFormat, kTargetFormat }));

        std::shared_ptr<Pipeline> compositePipeline = helper::createPipeline(
            device, compositeVertexFunction, compositeFragmentFunction,
            RenderTargetFormat::singleTarget(surface->getFormat()));

        std::shared_ptr<Pipeline> compositePipelineMRT = helper::createPipeline(
            device, compositeVertexFunction, compositeFragmentMRTFunction,
            RenderTargetFormat::singleTarget(surface->getFormat()));

        const Vertex quadVertices[] = {
                {{-1.0f, -1.0f}}, {{-1.0f,  1.0f}}, {{ 1.0f,  1.0f}}, {{ 1.0f, -1.0f}}
        };
        const uint16_t quadIndices[] = { 0, 1, 2, 0, 2, 3 };

        std::shared_ptr<GBuffer> vertexBuffer = device->createBuffer(
            BufferType::Vertex, BufferUsage::Static, sizeof(quadVertices), quadVertices, "GridVertexBuffer");
        std::shared_ptr<GBuffer> indexBuffer = device->createBuffer(
            BufferType::Index, BufferUsage::Static, sizeof(quadIndices), quadIndices, "GridIndexBuffer");
        std::shared_ptr<GBuffer> uniformBuffer = device->createBuffer(
            BufferType::Uniform, BufferUsage::Dynamic, sizeof(Uniforms), nullptr, "GridUniformBuffer");
        std::shared_ptr<GBuffer> compositeUniformBuffer = device->createBuffer(
            BufferType::Uniform, BufferUsage::Dynamic, sizeof(CompositeUniforms), nullptr, "CompositeUniformBuffer");

        // Один сэмплер на оба прохода: Nearest, чтобы промежуточная цель попадала в свопчейн
        // пиксель в пиксель и картинка не отличалась от прямого рендеринга.
        std::shared_ptr<GSampler> targetSampler = device->createSampler(
            SamplerDesc{ SamplerFilter::Nearest, SamplerFilter::Nearest },
            "TargetSampler");

        // --- Оффскрин-цели. Пересоздаются при изменении размера окна. ---
        std::shared_ptr<GImage> sceneTarget;
        std::shared_ptr<GImage> distanceTarget;
        int targetWidth = 0;
        int targetHeight = 0;

        auto ensureTargets = [&](int width, int height) {
            if (width == targetWidth && height == targetHeight && sceneTarget && distanceTarget) {
                return;
            }
            const ImageUsage usage = ImageUsage::ColorTarget | ImageUsage::Sampled;
            sceneTarget = device->createImage(ImageType::Image2D, kTargetFormat,
                                              static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                              usage, "SceneTarget");
            distanceTarget = device->createImage(ImageType::Image2D, kTargetFormat,
                                                 static_cast<uint32_t>(width), static_cast<uint32_t>(height),
                                                 usage, "DistanceTarget");
            targetWidth = width;
            targetHeight = height;
        };

        float viewScale = 1.0f;
        float viewPan[2] = { 0.0f, 0.0f };
        bool useMultipleTargets = true;
        float splitFraction = 0.5f;

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();

            std::shared_ptr<GImage> nextImgToDraw = swapChain->acquireNextImage();
            if (!nextImgToDraw) { continue; }

            glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
            if (fbWidth == 0 || fbHeight == 0) { continue; }
            ensureTargets(fbWidth, fbHeight);

            // --- ImGui ---
            std::shared_ptr<RenderPassDescriptor> compositePass = helper::createRenderPassDescriptor();
            ClearValue clearColor = { 0.98f, 0.98f, 0.96f, 1.0f };
            compositePass->setColorAttachment(0, nextImgToDraw, true, clearColor);

            helper::newFrameImgui(compositePass);
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            {
                ImGui::Begin("Controls");
                ImGui::Text("Viewport: %.0f x %.0f", (float)fbWidth, (float)fbHeight);
                ImGui::SliderFloat("Zoom", &viewScale, 0.1f, 10.0f);
                ImGui::SliderFloat2("Pan", &viewPan[0], -500.0f, 500.0f);
                if (ImGui::Button("Reset View")) {
                    viewScale = 1.0f;
                    viewPan[0] = viewPan[1] = 0.0f;
                }
                ImGui::Separator();
                ImGui::Checkbox("Multiple render targets", &useMultipleTargets);
                if (useMultipleTargets) {
                    ImGui::TextWrapped("Pass 1 writes the grid and a distance field into two "
                                       "textures at once; pass 2 samples both.");
                    ImGui::SliderFloat("Split", &splitFraction, 0.0f, 1.0f);
                } else {
                    ImGui::TextWrapped("Pass 1 writes the grid into a single texture; "
                                       "pass 2 samples it.");
                }
                ImGui::End();
            }
            ImGui::Render();

            Uniforms uniforms = { {(float)fbWidth, (float)fbHeight}, viewScale, {viewPan[0], viewPan[1]} };
            uniformBuffer->update(&uniforms, sizeof(Uniforms));

            CompositeUniforms compositeUniforms = {
                {(float)fbWidth, (float)fbHeight}, splitFraction * (float)fbWidth };
            compositeUniformBuffer->update(&compositeUniforms, sizeof(CompositeUniforms));

            std::shared_ptr<CommandBuffer> buffer = helper::createCommandBuffer(cmdLists);

            // --- Проход 1: рисуем в оффскрин-цели, одну или сразу две. ---
            {
                std::shared_ptr<RenderPassDescriptor> planePass = helper::createRenderPassDescriptor();
                planePass->setColorAttachment(0, sceneTarget, true, clearColor);
                if (useMultipleTargets) {
                    ClearValue distanceClear = { 0.0f, 0.0f, 0.0f, 1.0f };
                    planePass->setColorAttachment(1, distanceTarget, true, distanceClear);
                }

                buffer->beginRenderPass(planePass);
                {
                    buffer->setRenderPipeline(useMultipleTargets ? planePipelineMRT : planePipeline);
                    buffer->setVertexBuffer(0, vertexBuffer);
                    buffer->setUniformBuffer(1, ShaderStage::Vertex, uniformBuffer);
                    buffer->setUniformBuffer(1, ShaderStage::Fragment, uniformBuffer);
                    buffer->drawIndexed(indexBuffer, IndexType::UInt16, sizeof(quadIndices) / sizeof(uint16_t), 1, 0, 0, 0);
                }
                buffer->endRenderPass();
            }

            // --- Проход 2: сводим результат в изображение свопчейна и поверх рисуем ImGui. ---
            {
                buffer->beginRenderPass(compositePass);
                {
                    buffer->setRenderPipeline(useMultipleTargets ? compositePipelineMRT : compositePipeline);
                    buffer->setVertexBuffer(0, vertexBuffer);
                    buffer->setTexture(0, ShaderStage::Fragment, sceneTarget, targetSampler);
                    if (useMultipleTargets) {
                        buffer->setUniformBuffer(1, ShaderStage::Fragment, compositeUniformBuffer);
                        buffer->setTexture(1, ShaderStage::Fragment, distanceTarget, targetSampler);
                    }
                    buffer->drawIndexed(indexBuffer, IndexType::UInt16, sizeof(quadIndices) / sizeof(uint16_t), 1, 0, 0, 0);
                }

                helper::renderInternalImgui(buffer);
                buffer->endRenderPass();
            }

            buffer->present(nextImgToDraw);
            buffer->commit();
        }

        helper::shutdownImgui();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
}
