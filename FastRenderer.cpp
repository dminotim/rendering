// ───────────────────────────────────────────────
// FastRenderer.cpp — model viewer
//
// Loads a model (.obj / .stl / .ply) and draws it with depth testing and anti-aliasing.
// The whole frame loop is written once against the abstractions in render_pipeline/ and runs
// on both backends without a single conditional-compilation directive.
// ───────────────────────────────────────────────
#include "FastRenderer.hpp"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <filesystem>
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
#include "mesh/Mesh.hpp"

namespace dmrender
{
    /// Colour target format. Linear, like the surface, so the shader writes what is displayed.
    constexpr ImageFormat kColorFormat = ImageFormat::BGRA8_UNORM;
    /// 32-bit float depth is supported as an attachment everywhere.
    constexpr ImageFormat kDepthFormat = ImageFormat::D32_FLOAT;

    /// Model used when the application is given no path.
    constexpr const char* kDefaultModel = "bunny.obj";

    /// 112 bytes: one matrix plus three vectors. Fits the guaranteed 128.
    struct SceneConstants {
        float modelViewProjection[16];   // 64
        float lightDirection[4];         // 16
        float baseColor[4];              // 16
        float cameraParams[4];           // 16
    };
    static_assert(sizeof(SceneConstants) <= kMaxPushConstantBytes,
                  "SceneConstants must fit the guaranteed push constant range");

    // ─────────────────────────────────────────────────────────────────────────
    // Minimal matrix maths. This is application code, not library code: the
    // abstraction deliberately knows nothing about matrices.
    // ─────────────────────────────────────────────────────────────────────────

    using Mat4 = std::array<float, 16>;   // column-major, as in GLSL and MSL

    Mat4 multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 result{};
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) {
                    sum += a[k * 4 + row] * b[column * 4 + k];
                }
                result[column * 4 + row] = sum;
            }
        }
        return result;
    }

    /**
     * @brief Right-handed perspective projection with a 0..1 depth range.
     *
     * No Y flip and no Z remap: the wrapper aligned clip space between the backends, so one
     * matrix works on both platforms.
     */
    Mat4 perspective(float fovYRadians, float aspect, float nearZ, float farZ)
    {
        const float f = 1.0f / std::tan(fovYRadians * 0.5f);
        Mat4 m{};
        m[0] = f / aspect;
        m[5] = f;
        m[10] = farZ / (nearZ - farZ);
        m[11] = -1.0f;
        m[14] = (nearZ * farZ) / (nearZ - farZ);
        return m;
    }

    Mat4 translation(float x, float y, float z)
    {
        Mat4 m{};
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        m[12] = x; m[13] = y; m[14] = z;
        return m;
    }

    Mat4 scale(float s)
    {
        Mat4 m{};
        m[0] = m[5] = m[10] = s;
        m[15] = 1.0f;
        return m;
    }

    Mat4 rotationX(float radians)
    {
        const float c = std::cos(radians), s = std::sin(radians);
        Mat4 m{};
        m[0] = 1.0f;
        m[5] = c;  m[6] = s;
        m[9] = -s; m[10] = c;
        m[15] = 1.0f;
        return m;
    }

    Mat4 rotationY(float radians)
    {
        const float c = std::cos(radians), s = std::sin(radians);
        Mat4 m{};
        m[0] = c;  m[2] = -s;
        m[5] = 1.0f;
        m[8] = s;  m[10] = c;
        m[15] = 1.0f;
        return m;
    }

    void FramebufferSizeCallback(GLFWwindow* window, int width, int height) {
        auto* ctx = static_cast<std::shared_ptr<SwapChain>*>(glfwGetWindowUserPointer(window));
        if (*(ctx)) {
            (*ctx)->recreate(width, height);
        }
    }

    void run_loop()
    {
        // ── Load the model before creating a device: no point opening a window without one ──
        // The executable lands in build/<config>/, so the model sits two levels above the
        // working directory when it is launched from there, and zero levels above when it is
        // launched from the repository root. Walk up a few levels rather than guessing which.
        std::filesystem::path modelPath = kDefaultModel;
        {
            std::filesystem::path prefix;
            for (int level = 0; level <= 4 && !std::filesystem::exists(modelPath); ++level) {
                prefix /= "..";
                modelPath = prefix / kDefaultModel;
            }
        }

        std::string loadError;
        const Mesh mesh = loadMesh(modelPath, loadError);
        if (mesh.empty()) {
            std::fprintf(stderr, "Failed to load model: %s\n", loadError.c_str());
            return;
        }

        std::fprintf(stderr,
                     "Model %s: format %s, %zu vertices, %zu triangles, %zu materials\n"
                     "  normals: %s, texture coordinates: %s\n",
                     modelPath.string().c_str(), mesh.sourceFormat.c_str(),
                     mesh.vertices.size(), mesh.indices.size() / 3, mesh.materials.size(),
                     mesh.hadNormals ? "from file" : "generated on load",
                     mesh.hadTexCoords ? "present" : "absent");

        if (!glfwInit()) return;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window = glfwCreateWindow(1280, 720, "Mesh Viewer", nullptr, nullptr);
        if (!window) { glfwTerminate(); return; }

        std::shared_ptr<Surface> surface = helper::createSurface(window, kColorFormat);
        std::shared_ptr<Device> device = helper::createDefaultDevice(surface);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForOther(window, true);

        std::shared_ptr<CommandQueue> cmdLists = helper::createCommandQueue(device);

        int fbWidth, fbHeight;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);

        std::shared_ptr<SwapChain> swapChain =
            helper::createSwapChain(device, cmdLists, surface, fbWidth, fbHeight);
        glfwSetWindowUserPointer(window, &swapChain);
        glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);

        helper::initImgui(swapChain);

        // ── Geometry in video memory ──
        // Static: written once, read every frame — repays the staging upload.
        std::shared_ptr<GBuffer> vertexBuffer = device->createBuffer(
            BufferType::Vertex, BufferUsage::Static,
            mesh.vertices.size() * sizeof(MeshVertex), mesh.vertices.data(), "MeshVertices");
        std::shared_ptr<GBuffer> indexBuffer = device->createBuffer(
            BufferType::Index, BufferUsage::Static,
            mesh.indices.size() * sizeof(uint32_t), mesh.indices.data(), "MeshIndices");

        // ── Material textures ──
        // Only textures the model actually names get loaded. bunny.obj has none, so the shader
        // gets a one-pixel stand-in and multiplies by white.
        SamplerDesc samplerDesc{};
        samplerDesc.addressU = SamplerAddressMode::Repeat;
        samplerDesc.addressV = SamplerAddressMode::Repeat;
        std::shared_ptr<GSampler> sampler = device->createSampler(samplerDesc, "MeshSampler");

        const uint8_t whitePixel[4] = { 255, 255, 255, 255 };
        ImageDesc whiteDesc{};
        whiteDesc.format = ImageFormat::RGBA8_UNORM;
        whiteDesc.width = 1;
        whiteDesc.height = 1;
        whiteDesc.usage = ImageUsage::Sampled;
        whiteDesc.debugName = "WhiteFallback";
        std::shared_ptr<GImage> whiteTexture = device->createImage(whiteDesc, whitePixel);

        // ── Shaders and pipelines ──
        const std::filesystem::path meshShaderPath = std::filesystem::path(SHADER_DIR) / "Mesh";
        std::shared_ptr<ShaderFunction> meshVertexFunction =
            helper::createShaderFunction(device, meshShaderPath, "mesh_vertex_shader");
        std::shared_ptr<ShaderFunction> meshFragmentFunction =
            helper::createShaderFunction(device, meshShaderPath, "mesh_fragment_shader");

        const SampleCount maxSamples = device->maxSupportedSampleCount();
        const SampleCount msaaSamples =
            static_cast<uint32_t>(maxSamples) >= 4 ? SampleCount::Four : maxSamples;

        // Sample count is baked into a pipeline, so an anti-aliasing toggle needs two prebuilt
        // variants rather than a state change at draw time.
        auto makeMeshPipeline = [&](SampleCount samples) {
            PipelineDesc desc{};
            desc.vertexFunction = meshVertexFunction;
            desc.fragmentFunction = meshFragmentFunction;
            desc.targetFormat = RenderTargetFormat::singleTarget(kColorFormat, kDepthFormat, samples);
            desc.depthStencil = DepthStencilState::depthTestAndWrite();
            desc.rasterizer.cullMode = CullMode::Back;
            desc.rasterizer.frontFace = FrontFace::CounterClockwise;
            desc.debugName = "MeshPipeline";
            return helper::createPipeline(device, desc);
        };

        std::shared_ptr<Pipeline> meshPipeline = makeMeshPipeline(SampleCount::One);
        std::shared_ptr<Pipeline> meshPipelineMSAA = makeMeshPipeline(msaaSamples);

        // ── Render targets, recreated when the window changes size ──
        std::shared_ptr<GImage> msaaColorTarget;
        std::shared_ptr<GImage> depthTarget;
        std::shared_ptr<GImage> msaaDepthTarget;
        int targetWidth = 0;
        int targetHeight = 0;

        auto ensureTargets = [&](int width, int height) {
            if (width == targetWidth && height == targetHeight && depthTarget) return;

            ImageDesc colorDesc{};
            colorDesc.format = kColorFormat;
            colorDesc.width = static_cast<uint32_t>(width);
            colorDesc.height = static_cast<uint32_t>(height);
            colorDesc.sampleCount = msaaSamples;
            colorDesc.usage = ImageUsage::ColorTarget;
            colorDesc.debugName = "MsaaColorTarget";
            msaaColorTarget = device->createImage(colorDesc);

            ImageDesc depthDesc{};
            depthDesc.format = kDepthFormat;
            depthDesc.width = static_cast<uint32_t>(width);
            depthDesc.height = static_cast<uint32_t>(height);
            depthDesc.usage = ImageUsage::DepthStencil;

            depthDesc.debugName = "DepthTarget";
            depthTarget = device->createImage(depthDesc);

            depthDesc.sampleCount = msaaSamples;
            depthDesc.debugName = "MsaaDepthTarget";
            msaaDepthTarget = device->createImage(depthDesc);

            targetWidth = width;
            targetHeight = height;
        };

        // ── Camera framed on the model's bounds ──
        // A model can be any scale, from fractions of a unit to thousands, so it is normalised
        // rather than the camera being tuned per file.
        const std::array<float, 3> meshCenter = mesh.center();
        const float meshExtent = mesh.boundsExtent();
        const float normalizeScale = meshExtent > 1e-6f ? 1.0f / meshExtent : 1.0f;

        float yaw = 0.6f;
        float pitch = 0.25f;
        float distance = 2.2f;
        bool useMsaa = msaaSamples != SampleCount::One;
        float ambientAmount = 1.0f;
        float lightYaw = 0.9f;
        float lightPitch = 0.8f;
        ClearValue clearColor = { 0.09f, 0.10f, 0.12f, 1.0f };

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();

            std::shared_ptr<GImage> nextImgToDraw = swapChain->acquireNextImage();
            if (!nextImgToDraw) { continue; }

            // Target sizes come from the swapchain, not the window. The swapchain picks its
            // extent from the surface capabilities, and during a resize that can disagree for a
            // frame with what the window system reports. Every attachment of a pass must match
            // the size of the swapchain image it is used with.
            fbWidth = static_cast<int>(swapChain->width());
            fbHeight = static_cast<int>(swapChain->height());
            if (fbWidth == 0 || fbHeight == 0) { continue; }
            ensureTargets(fbWidth, fbHeight);

            // ── User interface ──
            // A separate pass over the scene. ImGui built its pipeline against the swapchain
            // configuration — one sample, no depth — and is incompatible inside a multisampled
            // pass with depth. Splitting "scene" from "interface" resolves that and also
            // reflects what these passes actually are.
            std::shared_ptr<RenderPassDescriptor> uiPass = helper::createRenderPassDescriptor();
            uiPass->setColorAttachment(0, nextImgToDraw, /*clear=*/false, clearColor);

            helper::newFrameImgui(uiPass);
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            {
                ImGui::Begin("Model");
                ImGui::Text("%s", modelPath.filename().string().c_str());
                ImGui::Text("Format: %s", mesh.sourceFormat.c_str());
                ImGui::Text("Vertices: %zu", mesh.vertices.size());
                ImGui::Text("Triangles: %zu", mesh.indices.size() / 3);
                ImGui::Text("Normals: %s", mesh.hadNormals ? "from file" : "generated");
                if (!mesh.materials.empty()) {
                    ImGui::Text("Materials: %zu", mesh.materials.size());
                }
                ImGui::Text("Bounds extent: %.3f", meshExtent);

                ImGui::Separator();
                ImGui::SliderFloat("Yaw", &yaw, -3.14159f, 3.14159f);
                ImGui::SliderFloat("Pitch", &pitch, -1.5f, 1.5f);
                ImGui::SliderFloat("Distance", &distance, 1.2f, 6.0f);

                ImGui::Separator();
                if (msaaSamples == SampleCount::One) {
                    ImGui::TextUnformatted("MSAA unsupported on this device");
                } else {
                    ImGui::Checkbox("MSAA", &useMsaa);
                    ImGui::SameLine();
                    ImGui::Text("(%ux)", static_cast<uint32_t>(msaaSamples));
                }

                ImGui::Separator();
                ImGui::SliderFloat("Ambient", &ambientAmount, 0.0f, 2.0f);
                ImGui::SliderFloat("Light yaw", &lightYaw, -3.14159f, 3.14159f);
                ImGui::SliderFloat("Light pitch", &lightPitch, -1.5f, 1.5f);

                ImGui::Separator();
                const MemoryBudget budget = device->queryMemoryBudget();
                ImGui::Text("VRAM %.0f / %.0f MiB",
                            budget.deviceLocalUsedBytes / 1048576.0,
                            budget.deviceLocalBudgetBytes / 1048576.0);
                ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
                ImGui::End();
            }
            ImGui::Render();

            // ── Matrices ──
            const float aspect = static_cast<float>(fbWidth) / static_cast<float>(fbHeight);
            const Mat4 projection = perspective(1.0472f /* 60 deg */, aspect, 0.05f, 100.0f);
            const Mat4 view = translation(0.0f, 0.0f, -distance);

            // Model: recentre, normalise scale, then rotate.
            const Mat4 recenter = translation(-meshCenter[0], -meshCenter[1], -meshCenter[2]);
            const Mat4 model = multiply(rotationY(yaw),
                                multiply(rotationX(pitch),
                                multiply(scale(normalizeScale), recenter)));

            SceneConstants constants{};
            const Mat4 mvp = multiply(projection, multiply(view, model));
            std::copy(mvp.begin(), mvp.end(), constants.modelViewProjection);

            constants.lightDirection[0] = std::cos(lightPitch) * std::sin(lightYaw);
            constants.lightDirection[1] = std::sin(lightPitch);
            constants.lightDirection[2] = std::cos(lightPitch) * std::cos(lightYaw);
            constants.cameraParams[0] = ambientAmount;

            // ── Pass 1: the scene ──
            std::shared_ptr<CommandBuffer> buffer = helper::createCommandBuffer(cmdLists);

            std::shared_ptr<RenderPassDescriptor> scenePass = helper::createRenderPassDescriptor();
            if (useMsaa) {
                // Draw into the multisample target; the pass resolves it into the swapchain
                // image on completion, which costs no extra draw.
                scenePass->setColorAttachment(0, msaaColorTarget, true, clearColor);
                scenePass->setResolveAttachment(0, nextImgToDraw);
                scenePass->setDepthStencilAttachment(msaaDepthTarget, true, 1.0f, false, 0);
            } else {
                scenePass->setColorAttachment(0, nextImgToDraw, true, clearColor);
                scenePass->setDepthStencilAttachment(depthTarget, true, 1.0f, false, 0);
            }

            buffer->beginRenderPass(scenePass);
            {
                buffer->setRenderPipeline(useMsaa ? meshPipelineMSAA : meshPipeline);
                buffer->setVertexBuffer(0, vertexBuffer);

                // One geometry buffer, one subset per material: switching material costs a push
                // constant write rather than rebinding buffers.
                for (const MeshSubset& subset : mesh.subsets) {
                    const MeshMaterial* material =
                        (subset.materialIndex >= 0 &&
                         subset.materialIndex < static_cast<int32_t>(mesh.materials.size()))
                            ? &mesh.materials[subset.materialIndex] : nullptr;

                    if (material) {
                        constants.baseColor[0] = material->baseColor[0];
                        constants.baseColor[1] = material->baseColor[1];
                        constants.baseColor[2] = material->baseColor[2];
                    } else {
                        constants.baseColor[0] = 0.78f;
                        constants.baseColor[1] = 0.76f;
                        constants.baseColor[2] = 0.72f;
                    }
                    // The loaded model has no textures — tell the shader there is nothing to
                    // multiply by.
                    constants.baseColor[3] = 0.0f;

                    buffer->setTexture(0, ShaderStage::Fragment, whiteTexture, sampler);
                    buffer->setPushConstants(ShaderStage::Vertex, &constants, sizeof(constants));
                    buffer->setPushConstants(ShaderStage::Fragment, &constants, sizeof(constants));

                    buffer->drawIndexed(indexBuffer, IndexType::UInt32,
                                        subset.indexCount, 1,
                                        subset.firstIndex * sizeof(uint32_t), 0, 0);
                }
            }
            buffer->endRenderPass();

            // ── Pass 2: the interface, over the finished frame ──
            // No clear: load whatever the scene pass left behind.
            buffer->beginRenderPass(uiPass);
            {
                helper::renderInternalImgui(buffer);
            }
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
