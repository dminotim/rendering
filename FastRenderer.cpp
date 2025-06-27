// ───────────────────────────────────────────────
// FastRenderer.mm
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
#include "Commandbuffer.hpp"
#include "CommandQueue.hpp"
#include "ShaderFunction.hpp"
#include "Pipeline.hpp"

#include "RenderHelper.hpp"

#include <simd/simd.h>

namespace dmrender
{
    struct Vertex {
        vector_float2 position;
    };

    struct Uniforms {
        float viewportSize[2];
        float scale;
        float pan[2];
    };

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

        std::shared_ptr<Device> device = helper::createDefaultDevice();

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForOther(window, true);
        helper::initImgui(device); // Инициализация ImGui через хелпер

        std::shared_ptr<Surface> surface = helper::createSurface(window, device, ImageFormat::BGRA8_UNORM);
        std::shared_ptr<CommandQueue> cmdLists = helper::createCommandQueue(device);

        int fbWidth, fbHeight;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);

        std::shared_ptr<SwapChain> swapChain = helper::createSwapChain(device, cmdLists, surface, fbWidth, fbHeight);
        glfwSetWindowUserPointer(window, &swapChain);
        glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);

        const std::filesystem::path shaderPath = std::filesystem::path(SHADER_DIR) / "PlaneShader.metal";
        std::shared_ptr<ShaderFunction> planeVertexFunction = helper::createShaderFunction(device, shaderPath,
                                                                                           "plane_vertex_shader");
        std::shared_ptr<ShaderFunction> planeFragmentFunction = helper::createShaderFunction(device, shaderPath,
                                                                                             "plane_fragment_shader");

        RenderTargetFormat targetFormatForPipeline{};
        targetFormatForPipeline.colorFormat = surface->getFormat();

        std::shared_ptr<Pipeline> pipeline = helper::createPipeline(device, planeVertexFunction, planeFragmentFunction, targetFormatForPipeline);

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

        float viewScale = 1.0f;
        float viewPan[2] = {0.0f, 0.0f};

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();

            std::shared_ptr<GImage> nextImgToDraw = swapChain->acquireNextImage();
            if (!nextImgToDraw) { continue; }

            std::shared_ptr<RenderPassDescriptor> renderPass = helper::createRenderPassDescriptor();
            ClearValue clearColor = {0.98f, 0.98f, 0.96f, 1.0f};
            renderPass->setColorAttachment(0, nextImgToDraw, true, clearColor);

            std::shared_ptr<CommandBuffer> buffer = helper::createCommandBuffer(cmdLists);

            glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
            Uniforms uniforms = {{(float)fbWidth, (float)fbHeight}, viewScale, {viewPan[0], viewPan[1]}};
            uniformBuffer->update(&uniforms, sizeof(Uniforms));

            buffer->beginRenderPass(renderPass);
            {
                buffer->setRenderPipeline(pipeline);
                buffer->setVertexBuffer(0, vertexBuffer);
                buffer->setUniformBuffer(1, ShaderStage::Vertex, uniformBuffer);
                buffer->setUniformBuffer(1, ShaderStage::Fragment, uniformBuffer);
                buffer->drawIndexed(indexBuffer, IndexType::UInt16, sizeof(quadIndices) / sizeof(uint16_t), 1, 0, 0, 0);
            }
            // Используем ImGui хелперы
            helper::newFrameImgui(renderPass);
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
                ImGui::End();
            }
            ImGui::Render();
            helper::renderInternalImgui(buffer);

            buffer->endRenderPass();
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