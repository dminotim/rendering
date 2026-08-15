// ───────────────────────────────────────────────
// FastRenderer.cpp
// ───────────────────────────────────────────────
#include "FastRenderer.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <memory>
#include <vector>

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

    /// Pushed straight into the command stream rather than living in a uniform buffer.
    struct OverlayConstants {
        float centerX;
        float centerY;
        float halfWidth;
        float halfHeight;
        float opacity;
    };

    /**
     * @brief Writes BGRA8 pixels out as a 32-bit BMP.
     *
     * BMP because it opens anywhere on Windows without a decoder, and because its byte order is
     * already BGRA — the same as the render targets — so the readback needs no channel swizzle.
     * A negative height stores the image top-down, matching the order the GPU hands it back.
     */
    bool writeBmp(const std::filesystem::path& path,
                  const std::vector<uint8_t>& bgraPixels,
                  uint32_t width,
                  uint32_t height)
    {
        std::ofstream file(path, std::ios::binary);
        if (!file) return false;

        const uint32_t pixelBytes = width * height * 4;
        const uint32_t fileHeaderSize = 14;
        const uint32_t infoHeaderSize = 40;
        const uint32_t offset = fileHeaderSize + infoHeaderSize;

        auto writeU16 = [&](uint16_t value) { file.write(reinterpret_cast<const char*>(&value), 2); };
        auto writeU32 = [&](uint32_t value) { file.write(reinterpret_cast<const char*>(&value), 4); };
        auto writeI32 = [&](int32_t value) { file.write(reinterpret_cast<const char*>(&value), 4); };

        file.write("BM", 2);
        writeU32(offset + pixelBytes);
        writeU16(0);
        writeU16(0);
        writeU32(offset);

        writeU32(infoHeaderSize);
        writeI32(static_cast<int32_t>(width));
        writeI32(-static_cast<int32_t>(height));   // negative: rows run top to bottom
        writeU16(1);
        writeU16(32);
        writeU32(0);                                // BI_RGB
        writeU32(pixelBytes);
        writeI32(2835);
        writeI32(2835);
        writeU32(0);
        writeU32(0);

        file.write(reinterpret_cast<const char*>(bgraPixels.data()), pixelBytes);
        return file.good();
    }

    /// Side of the procedurally generated overlay texture, in texels.
    constexpr uint32_t kOverlayTextureSize = 256;

    /**
     * @brief Builds a checkerboard with a radial alpha falloff, tightly packed as RGBA8.
     *
     * The fine checker is deliberately high frequency: drawn at overlay size it is minified
     * several times over, so it looks smooth only if mip levels are being sampled. The alpha
     * falloff makes the blend state visible — with blending off the quad's edges would be a
     * hard rectangle instead of fading out.
     */
    std::vector<uint8_t> makeOverlayPixels(uint32_t size)
    {
        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        const float centre = static_cast<float>(size) * 0.5f;

        for (uint32_t y = 0; y < size; ++y) {
            for (uint32_t x = 0; x < size; ++x) {
                const bool light = (((x / 8) + (y / 8)) % 2) == 0;

                const float dx = (static_cast<float>(x) - centre) / centre;
                const float dy = (static_cast<float>(y) - centre) / centre;
                const float distance = std::sqrt(dx * dx + dy * dy);
                const float alpha = std::clamp(1.0f - distance, 0.0f, 1.0f);

                uint8_t* texel = &pixels[(static_cast<size_t>(y) * size + x) * 4];
                texel[0] = light ? 250 : 40;    // R
                texel[1] = light ? 190 : 90;    // G
                texel[2] = light ? 60 : 190;    // B
                texel[3] = static_cast<uint8_t>(alpha * 255.0f);
            }
        }
        return pixels;
    }

    /// Pixel format of the offscreen render targets. Linear, like the swapchain, so a value
    /// written by the plane pass survives the round trip through the composite pass untouched.
    constexpr ImageFormat kTargetFormat = ImageFormat::BGRA8_UNORM;

    const char* toString(MemoryLocation location) {
        return location == MemoryLocation::DeviceLocal ? "VRAM" : "host";
    }

    double toMiB(uint64_t bytes) {
        return static_cast<double>(bytes) / (1024.0 * 1024.0);
    }

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

        // A pipeline's sample count is baked in, so anti-aliasing cannot be toggled on an
        // existing one — each combination needs its own. Four samples is the count every
        // desktop GPU supports; clamp to what this one actually offers.
        const SampleCount maxSamples = device->maxSupportedSampleCount();
        const SampleCount msaaSamples =
            static_cast<uint32_t>(maxSamples) >= 4 ? SampleCount::Four : maxSamples;

        std::shared_ptr<Pipeline> planePipelineMSAA = helper::createPipeline(
            device, planeVertexFunction, planeFragmentFunction,
            RenderTargetFormat::singleTarget(kTargetFormat, ImageFormat::Undefined, msaaSamples));

        std::shared_ptr<Pipeline> planePipelineMRTMSAA = helper::createPipeline(
            device, planeVertexFunction, planeFragmentMRTFunction,
            RenderTargetFormat::multiTarget({ kTargetFormat, kTargetFormat },
                                            ImageFormat::Undefined, msaaSamples));

        std::shared_ptr<Pipeline> compositePipeline = helper::createPipeline(
            device, compositeVertexFunction, compositeFragmentFunction,
            RenderTargetFormat::singleTarget(surface->getFormat()));

        std::shared_ptr<Pipeline> compositePipelineMRT = helper::createPipeline(
            device, compositeVertexFunction, compositeFragmentMRTFunction,
            RenderTargetFormat::singleTarget(surface->getFormat()));

        // The overlay is the one pipeline that needs non-default fixed-function state: it
        // composites over what the composite pass already wrote instead of replacing it.
        const std::filesystem::path overlayPath = std::filesystem::path(SHADER_DIR) / "Overlay";
        PipelineDesc overlayDesc{};
        overlayDesc.vertexFunction = helper::createShaderFunction(device, overlayPath, "overlay_vertex_shader");
        overlayDesc.fragmentFunction = helper::createShaderFunction(device, overlayPath, "overlay_fragment_shader");
        overlayDesc.targetFormat = RenderTargetFormat::singleTarget(surface->getFormat());
        overlayDesc.blendStates = { BlendState::alphaBlend() };
        overlayDesc.debugName = "OverlayPipeline";
        std::shared_ptr<Pipeline> overlayPipeline = helper::createPipeline(device, overlayDesc);

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
        // A CPU-generated texture, uploaded through the staging path and given a full mip chain.
        const std::vector<uint8_t> overlayPixels = makeOverlayPixels(kOverlayTextureSize);
        ImageDesc overlayTextureDesc{};
        overlayTextureDesc.format = ImageFormat::RGBA8_UNORM;
        overlayTextureDesc.width = kOverlayTextureSize;
        overlayTextureDesc.height = kOverlayTextureSize;
        overlayTextureDesc.mipLevels = kFullMipChain;
        overlayTextureDesc.usage = ImageUsage::Sampled;
        overlayTextureDesc.debugName = "OverlayTexture";
        std::shared_ptr<GImage> overlayTexture = device->createImage(overlayTextureDesc, overlayPixels.data());

        // Один сэмплер на оба прохода: Nearest, чтобы промежуточная цель попадала в свопчейн
        // пиксель в пиксель и картинка не отличалась от прямого рендеринга.
        SamplerDesc targetSamplerDesc{};
        targetSamplerDesc.minFilter = SamplerFilter::Nearest;
        targetSamplerDesc.magFilter = SamplerFilter::Nearest;
        std::shared_ptr<GSampler> targetSampler = device->createSampler(targetSamplerDesc, "TargetSampler");

        // Trilinear, so the overlay's mip chain is actually blended across levels rather than
        // snapping between them.
        SamplerDesc overlaySamplerDesc{};
        std::shared_ptr<GSampler> overlaySampler = device->createSampler(overlaySamplerDesc, "OverlaySampler");

        // --- Оффскрин-цели. Пересоздаются при изменении размера окна. ---
        std::shared_ptr<GImage> sceneTarget;
        std::shared_ptr<GImage> distanceTarget;
        std::shared_ptr<GImage> msaaSceneTarget;
        std::shared_ptr<GImage> msaaDistanceTarget;
        int targetWidth = 0;
        int targetHeight = 0;

        auto ensureTargets = [&](int width, int height) {
            if (width == targetWidth && height == targetHeight && sceneTarget && distanceTarget) {
                return;
            }
            ImageDesc targetDesc{};
            targetDesc.format = kTargetFormat;
            targetDesc.width = static_cast<uint32_t>(width);
            targetDesc.height = static_cast<uint32_t>(height);
            // TransferSrc so the screenshot below can read it back.
            targetDesc.usage = ImageUsage::ColorTarget | ImageUsage::Sampled | ImageUsage::TransferSrc;

            targetDesc.debugName = "SceneTarget";
            sceneTarget = device->createImage(targetDesc);

            targetDesc.debugName = "DistanceTarget";
            distanceTarget = device->createImage(targetDesc);

            // The multisample pair the plane pass renders into when MSAA is on. They are never
            // sampled or read back — only resolved into the two targets above — so they carry
            // neither Sampled nor TransferSrc, which the backend enforces.
            ImageDesc msaaDesc{};
            msaaDesc.format = kTargetFormat;
            msaaDesc.width = static_cast<uint32_t>(width);
            msaaDesc.height = static_cast<uint32_t>(height);
            msaaDesc.sampleCount = msaaSamples;
            msaaDesc.usage = ImageUsage::ColorTarget;

            msaaDesc.debugName = "MsaaSceneTarget";
            msaaSceneTarget = device->createImage(msaaDesc);

            msaaDesc.debugName = "MsaaDistanceTarget";
            msaaDistanceTarget = device->createImage(msaaDesc);

            targetWidth = width;
            targetHeight = height;
        };

        float viewScale = 1.0f;
        float viewPan[2] = { 0.0f, 0.0f };
        bool useMultipleTargets = true;
        float splitFraction = 0.5f;
        bool showOverlay = true;
        float overlayOpacity = 0.85f;
        float overlayScale = 0.25f;
        bool useMsaa = msaaSamples != SampleCount::One;
        bool requestScreenshot = false;
        std::string screenshotStatus;

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
                const MemoryBudget budget = device->queryMemoryBudget();
                ImGui::Text("VRAM %.0f / %.0f MiB used%s",
                            toMiB(budget.deviceLocalUsedBytes),
                            toMiB(budget.deviceLocalBudgetBytes),
                            budget.preciseBudget ? "" : " (estimated)");
                if (budget.nativeAllocationCount > 0) {
                    ImGui::Text("%u native allocations, %.0f MiB reserved",
                                budget.nativeAllocationCount, toMiB(budget.reservedBytes));
                }
                if (budget.unifiedMemory) {
                    ImGui::TextUnformatted("Unified memory: staging copies are same-pool");
                }
                ImGui::Text("vertices %s | indices %s | uniforms %s",
                            toString(vertexBuffer->memoryLocation()),
                            toString(indexBuffer->memoryLocation()),
                            toString(uniformBuffer->memoryLocation()));

                ImGui::Separator();
                ImGui::Checkbox("Alpha-blended overlay", &showOverlay);
                if (showOverlay) {
                    ImGui::Text("Uploaded texture: %ux%u, %u mip levels",
                                overlayTexture->width(), overlayTexture->height(),
                                overlayTexture->mipLevels());
                    ImGui::SliderFloat("Opacity", &overlayOpacity, 0.0f, 1.0f);
                    ImGui::SliderFloat("Size", &overlayScale, 0.05f, 1.0f);
                }

                ImGui::Separator();
                if (msaaSamples == SampleCount::One) {
                    ImGui::TextUnformatted("MSAA unsupported on this device");
                } else {
                    ImGui::Checkbox("MSAA", &useMsaa);
                    ImGui::SameLine();
                    ImGui::Text("(%ux, max %ux)",
                                static_cast<uint32_t>(msaaSamples), static_cast<uint32_t>(maxSamples));
                }

                if (ImGui::Button("Save screenshot")) {
                    requestScreenshot = true;
                }
                if (!screenshotStatus.empty()) {
                    ImGui::TextUnformatted(screenshotStatus.c_str());
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
                ClearValue distanceClear = { 0.0f, 0.0f, 0.0f, 1.0f };

                if (useMsaa) {
                    // Render into the multisample images and let the pass resolve them into the
                    // ordinary ones, which is what the composite pass samples. The resolve is
                    // part of ending the pass, so it costs no extra draw.
                    planePass->setColorAttachment(0, msaaSceneTarget, true, clearColor);
                    planePass->setResolveAttachment(0, sceneTarget);
                    if (useMultipleTargets) {
                        planePass->setColorAttachment(1, msaaDistanceTarget, true, distanceClear);
                        planePass->setResolveAttachment(1, distanceTarget);
                    }
                } else {
                    planePass->setColorAttachment(0, sceneTarget, true, clearColor);
                    if (useMultipleTargets) {
                        planePass->setColorAttachment(1, distanceTarget, true, distanceClear);
                    }
                }

                buffer->beginRenderPass(planePass);
                {
                    std::shared_ptr<Pipeline> planeState =
                        useMultipleTargets ? (useMsaa ? planePipelineMRTMSAA : planePipelineMRT)
                                           : (useMsaa ? planePipelineMSAA : planePipeline);
                    buffer->setRenderPipeline(planeState);
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

                // Drawn inside the same pass, so alpha blending composites it against the pixels
                // the composite draw just produced rather than needing another pass.
                if (showOverlay) {
                    const float aspect = (float)fbHeight / (float)fbWidth;
                    OverlayConstants overlayConstants{};
                    overlayConstants.halfWidth = overlayScale * aspect;
                    overlayConstants.halfHeight = overlayScale;
                    overlayConstants.centerX = 1.0f - overlayConstants.halfWidth - 0.04f;
                    overlayConstants.centerY = 1.0f - overlayConstants.halfHeight - 0.04f;
                    overlayConstants.opacity = overlayOpacity;

                    buffer->setRenderPipeline(overlayPipeline);
                    buffer->setVertexBuffer(0, vertexBuffer);
                    buffer->setTexture(0, ShaderStage::Fragment, overlayTexture, overlaySampler);

                    // 20 bytes of per-draw data with no buffer, no descriptor, no allocation.
                    buffer->setPushConstants(ShaderStage::Vertex, &overlayConstants, sizeof(overlayConstants));
                    buffer->setPushConstants(ShaderStage::Fragment, &overlayConstants, sizeof(overlayConstants));
                    buffer->drawIndexed(indexBuffer, IndexType::UInt16, sizeof(quadIndices) / sizeof(uint16_t), 1, 0, 0, 0);
                }

                helper::renderInternalImgui(buffer);
                buffer->endRenderPass();
            }

            buffer->present(nextImgToDraw);
            buffer->commit();

            // After commit, so the frame being captured has actually been submitted. readback()
            // waits for the GPU internally, which is why this is a button rather than something
            // the loop does every frame.
            if (requestScreenshot) {
                requestScreenshot = false;
                try {
                    const uint32_t width = sceneTarget->width();
                    const uint32_t height = sceneTarget->height();
                    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height *
                                                bytesPerPixel(sceneTarget->format()));
                    sceneTarget->readback(pixels.data(), pixels.size());

                    const std::filesystem::path path =
                        std::filesystem::current_path() / "screenshot.bmp";
                    screenshotStatus = writeBmp(path, pixels, width, height)
                        ? "Saved " + path.string()
                        : "Failed to write " + path.string();
                }
                catch (const std::exception& e) {
                    screenshotStatus = std::string("Readback failed: ") + e.what();
                }
            }
        }

        helper::shutdownImgui();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
}
