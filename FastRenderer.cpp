// ───────────────────────────────────────────────
// FastRenderer.cpp — просмотрщик моделей
//
// Загружает модель (.obj / .stl / .ply) и рисует её с тестом глубины и сглаживанием.
// Весь цикл кадра написан один раз поверх абстракций из render_pipeline/ и работает
// на обоих бэкендах без единой директивы условной компиляции.
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
    /// Формат цветовой цели. Линейный, как и у поверхности, — шейдер пишет то, что видно.
    constexpr ImageFormat kColorFormat = ImageFormat::BGRA8_UNORM;
    /// 32-битная плавающая глубина поддерживается как цель везде.
    constexpr ImageFormat kDepthFormat = ImageFormat::D32_FLOAT;

    /// Модель по умолчанию, если приложению не передали путь.
    constexpr const char* kDefaultModel = "bunny.obj";

    /// 112 байт: матрица плюс три вектора. Укладывается в гарантированные 128.
    struct SceneConstants {
        float modelViewProjection[16];   // 64
        float lightDirection[4];         // 16
        float baseColor[4];              // 16
        float cameraParams[4];           // 16
    };
    static_assert(sizeof(SceneConstants) <= kMaxPushConstantBytes,
                  "SceneConstants must fit the guaranteed push constant range");

    // ─────────────────────────────────────────────────────────────────────────
    // Минимальная матричная математика. Это код приложения, а не библиотеки:
    // абстракция намеренно ничего не знает про матрицы.
    // ─────────────────────────────────────────────────────────────────────────

    using Mat4 = std::array<float, 16>;   // по столбцам, как в GLSL и MSL

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
     * @brief Правосторонняя перспектива с диапазоном глубины 0..1.
     *
     * Ни переворота Y, ни пересчёта Z: обёртка выравняла пространство отсечения между
     * бэкендами, поэтому одна матрица работает на обеих платформах.
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
        // ── Загрузка модели до создания устройства: если её нет, незачем открывать окно ──
        std::filesystem::path modelPath = kDefaultModel;
        if (!std::filesystem::exists(modelPath)) {
            // Запуск из каталога сборки — типичный случай, поэтому пробуем и уровнем выше.
            modelPath = std::filesystem::path("..") / kDefaultModel;
        }

        std::string loadError;
        const Mesh mesh = loadMesh(modelPath, loadError);
        if (mesh.empty()) {
            std::fprintf(stderr, "Не удалось загрузить модель: %s\n", loadError.c_str());
            return;
        }

        std::fprintf(stderr,
                     "Модель %s: формат %s, %zu вершин, %zu треугольников, %zu материалов\n"
                     "  нормали: %s, текстурные координаты: %s\n",
                     modelPath.string().c_str(), mesh.sourceFormat.c_str(),
                     mesh.vertices.size(), mesh.indices.size() / 3, mesh.materials.size(),
                     mesh.hadNormals ? "из файла" : "построены при загрузке",
                     mesh.hadTexCoords ? "есть" : "нет");

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

        // ── Геометрия в видеопамяти ──
        // Static: пишется один раз, читается каждый кадр — окупает загрузку через
        // промежуточный буфер.
        std::shared_ptr<GBuffer> vertexBuffer = device->createBuffer(
            BufferType::Vertex, BufferUsage::Static,
            mesh.vertices.size() * sizeof(MeshVertex), mesh.vertices.data(), "MeshVertices");
        std::shared_ptr<GBuffer> indexBuffer = device->createBuffer(
            BufferType::Index, BufferUsage::Static,
            mesh.indices.size() * sizeof(uint32_t), mesh.indices.data(), "MeshIndices");

        // ── Текстуры материалов ──
        // Загружаются только те, что модель действительно называет. У bunny.obj их нет,
        // поэтому шейдер получит однопиксельную заглушку и умножит на белое.
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

        // ── Шейдеры и пайплайны ──
        const std::filesystem::path meshShaderPath = std::filesystem::path(SHADER_DIR) / "Mesh";
        std::shared_ptr<ShaderFunction> meshVertexFunction =
            helper::createShaderFunction(device, meshShaderPath, "mesh_vertex_shader");
        std::shared_ptr<ShaderFunction> meshFragmentFunction =
            helper::createShaderFunction(device, meshShaderPath, "mesh_fragment_shader");

        const SampleCount maxSamples = device->maxSupportedSampleCount();
        const SampleCount msaaSamples =
            static_cast<uint32_t>(maxSamples) >= 4 ? SampleCount::Four : maxSamples;

        // Число сэмплов запекается в пайплайн, поэтому переключатель сглаживания требует
        // двух готовых вариантов, а не изменения состояния на лету.
        auto makeMeshPipeline = [&](SampleCount samples, PolygonMode polygonMode) {
            PipelineDesc desc{};
            desc.vertexFunction = meshVertexFunction;
            desc.fragmentFunction = meshFragmentFunction;
            desc.targetFormat = RenderTargetFormat::singleTarget(kColorFormat, kDepthFormat, samples);
            desc.depthStencil = DepthStencilState::depthTestAndWrite();
            desc.rasterizer.cullMode = CullMode::Back;
            desc.rasterizer.frontFace = FrontFace::CounterClockwise;
            desc.rasterizer.polygonMode = polygonMode;
            desc.debugName = "MeshPipeline";
            return helper::createPipeline(device, desc);
        };

        std::shared_ptr<Pipeline> meshPipeline = makeMeshPipeline(SampleCount::One, PolygonMode::Fill);
        std::shared_ptr<Pipeline> meshPipelineMSAA = makeMeshPipeline(msaaSamples, PolygonMode::Fill);

        // ── Цели рендеринга, пересоздаваемые при изменении размера окна ──
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

        // ── Камера, подогнанная под габариты модели ──
        // Модель может быть любого масштаба — от долей единицы до тысяч, — поэтому её
        // нормализуем, а не подбираем камеру под каждый файл.
        const std::array<float, 3> meshCenter = mesh.center();
        const float meshExtent = mesh.boundsExtent();
        const float normalizeScale = meshExtent > 1e-6f ? 1.0f / meshExtent : 1.0f;

        float yaw = 0.6f;
        float pitch = 0.25f;
        float distance = 2.2f;
        bool autoRotate = true;
        bool useMsaa = msaaSamples != SampleCount::One;
        float ambientAmount = 1.0f;
        float lightYaw = 0.9f;
        float lightPitch = 0.8f;
        ClearValue clearColor = { 0.09f, 0.10f, 0.12f, 1.0f };

        double lastTime = glfwGetTime();

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();

            std::shared_ptr<GImage> nextImgToDraw = swapChain->acquireNextImage();
            if (!nextImgToDraw) { continue; }

            // Размеры целей берутся у свопчейна, а не у окна. Свопчейн выбирает протяжённость
            // по возможностям поверхности, и во время изменения размера она может на кадр
            // разойтись с тем, что сообщает оконная система. Любое вложение прохода обязано
            // совпадать по размеру с изображением свопчейна, с которым оно используется.
            fbWidth = static_cast<int>(swapChain->width());
            fbHeight = static_cast<int>(swapChain->height());
            if (fbWidth == 0 || fbHeight == 0) { continue; }
            ensureTargets(fbWidth, fbHeight);

            const double now = glfwGetTime();
            const float deltaTime = static_cast<float>(now - lastTime);
            lastTime = now;
            if (autoRotate) yaw += deltaTime * 0.4f;

            // ── Интерфейс ──
            // Отдельный проход поверх сцены. ImGui собрал свой пайплайн под конфигурацию
            // свопчейна — один сэмпл, без глубины, — и внутри многосэмплового прохода со
            // глубиной он несовместим. Разделение на «сцена» и «интерфейс» решает это и
            // заодно отражает то, чем эти проходы являются.
            std::shared_ptr<RenderPassDescriptor> uiPass = helper::createRenderPassDescriptor();
            uiPass->setColorAttachment(0, nextImgToDraw, /*clear=*/false, clearColor);

            helper::newFrameImgui(uiPass);
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            {
                ImGui::Begin("Model");
                ImGui::Text("%s", modelPath.filename().string().c_str());
                ImGui::Text("Формат: %s", mesh.sourceFormat.c_str());
                ImGui::Text("Вершин: %zu", mesh.vertices.size());
                ImGui::Text("Треугольников: %zu", mesh.indices.size() / 3);
                ImGui::Text("Нормали: %s", mesh.hadNormals ? "из файла" : "построены");
                if (!mesh.materials.empty()) {
                    ImGui::Text("Материалов: %zu", mesh.materials.size());
                }
                ImGui::Text("Габарит: %.3f", meshExtent);

                ImGui::Separator();
                ImGui::Checkbox("Вращение", &autoRotate);
                ImGui::SliderFloat("Поворот", &yaw, -3.14159f, 3.14159f);
                ImGui::SliderFloat("Наклон", &pitch, -1.5f, 1.5f);
                ImGui::SliderFloat("Дистанция", &distance, 1.2f, 6.0f);

                ImGui::Separator();
                if (msaaSamples == SampleCount::One) {
                    ImGui::TextUnformatted("Сглаживание недоступно");
                } else {
                    ImGui::Checkbox("Сглаживание", &useMsaa);
                    ImGui::SameLine();
                    ImGui::Text("(%ux)", static_cast<uint32_t>(msaaSamples));
                }

                ImGui::Separator();
                ImGui::SliderFloat("Окружение", &ambientAmount, 0.0f, 2.0f);
                ImGui::SliderFloat("Свет: поворот", &lightYaw, -3.14159f, 3.14159f);
                ImGui::SliderFloat("Свет: наклон", &lightPitch, -1.5f, 1.5f);

                ImGui::Separator();
                const MemoryBudget budget = device->queryMemoryBudget();
                ImGui::Text("VRAM %.0f / %.0f MiB",
                            budget.deviceLocalUsedBytes / 1048576.0,
                            budget.deviceLocalBudgetBytes / 1048576.0);
                ImGui::Text("%.1f FPS", ImGui::GetIO().Framerate);
                ImGui::End();
            }
            ImGui::Render();

            // ── Матрицы ──
            const float aspect = static_cast<float>(fbWidth) / static_cast<float>(fbHeight);
            const Mat4 projection = perspective(1.0472f /* 60° */, aspect, 0.05f, 100.0f);
            const Mat4 view = translation(0.0f, 0.0f, -distance);

            // Модель: центрируем, нормализуем масштаб, затем вращаем.
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

            // ── Проход 1: сцена ──
            std::shared_ptr<CommandBuffer> buffer = helper::createCommandBuffer(cmdLists);

            std::shared_ptr<RenderPassDescriptor> scenePass = helper::createRenderPassDescriptor();
            if (useMsaa) {
                // Рисуем в многосэмпловую цель, а проход сводит её в изображение свопчейна
                // при завершении — отдельной отрисовки это не стоит.
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

                // Один буфер геометрии, по подмножеству на материал: смена материала стоит
                // записи push-констант, а не пересвязывания буферов.
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
                    // Текстур у загруженной модели нет — сообщаем шейдеру, что умножать не на что.
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

            // ── Проход 2: интерфейс поверх готового кадра ──
            // Без очистки: загружаем то, что оставил проход сцены.
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
