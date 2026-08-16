// ───────────────────────────────────────────────
// FastRenderer.cpp
// ───────────────────────────────────────────────
#include "FastRenderer.hpp"
#include <GLFW/glfw3.h>
#include <algorithm>
#include <array>
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

    /// A cube vertex, padded to 16 bytes so it matches std430's vec3 array stride.
    struct Vertex3D {
        float position[3];
        float pad = 0.0f;
    };

    /// Pushed straight into the command stream: 64 bytes of matrix plus 16 of parameters.
    struct SceneConstants {
        float modelViewProjection[16];
        float volumeMix;
        float pad0 = 0.0f;
        float pad1 = 0.0f;
        float pad2 = 0.0f;
    };

    /// Side of the procedurally generated volume texture, in voxels.
    constexpr uint32_t kVolumeSize = 32;
    /// Side of each cubemap face, in texels.
    constexpr uint32_t kCubeFaceSize = 64;

    /// Cube instances drawn from a single indirect command, arranged 3x3.
    constexpr uint32_t kCubeInstances = 9;

    // --- Just enough matrix maths for the demo; this is application code, not library code. ---

    using Mat4 = std::array<float, 16>;  // column-major, matching GLSL and MSL

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
     * @brief Right-handed perspective projection producing a 0..1 depth range.
     *
     * Both backends use 0..1 clip-space Z — Vulkan natively, Metal likewise — so one projection
     * serves both. The Y axis needs no flip either, because the Vulkan backend renders with an
     * inverted viewport to match Metal's clip space. See ShaderFunction.hpp.
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

    /**
     * @brief A 3D texture whose colour varies independently along all three axes.
     *
     * Deliberately different in every direction so that a wrong slice pitch, a collapsed depth,
     * or a volume silently treated as 2D is visible rather than plausible.
     */
    std::vector<uint8_t> makeVolumePixels(uint32_t size)
    {
        std::vector<uint8_t> voxels(static_cast<size_t>(size) * size * size * 4);
        for (uint32_t z = 0; z < size; ++z) {
            for (uint32_t y = 0; y < size; ++y) {
                for (uint32_t x = 0; x < size; ++x) {
                    const size_t index = ((static_cast<size_t>(z) * size + y) * size + x) * 4;
                    voxels[index + 0] = static_cast<uint8_t>(255 * x / (size - 1));
                    voxels[index + 1] = static_cast<uint8_t>(255 * y / (size - 1));
                    voxels[index + 2] = static_cast<uint8_t>(255 * z / (size - 1));
                    voxels[index + 3] = 255;
                }
            }
        }
        return voxels;
    }

    /**
     * @brief One flat, distinctly coloured face of a cubemap.
     *
     * Flat colours because the point is to prove each of the six layers received its own upload;
     * a gradient would make an off-by-one in the face order hard to spot.
     */
    std::vector<uint8_t> makeCubeFacePixels(uint32_t size, uint32_t face)
    {
        static const uint8_t faceColors[6][3] = {
            { 220,  60,  60 },   // +X red
            {  90,  30,  30 },   // -X dark red
            {  60, 220,  60 },   // +Y green
            {  30,  90,  30 },   // -Y dark green
            {  60,  60, 220 },   // +Z blue
            {  30,  30,  90 },   // -Z dark blue
        };

        std::vector<uint8_t> pixels(static_cast<size_t>(size) * size * 4);
        for (size_t i = 0; i < static_cast<size_t>(size) * size; ++i) {
            pixels[i * 4 + 0] = faceColors[face][0];
            pixels[i * 4 + 1] = faceColors[face][1];
            pixels[i * 4 + 2] = faceColors[face][2];
            pixels[i * 4 + 3] = 255;
        }
        return pixels;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // A minimal BC3 encoder.
    //
    // Real projects compress offline with a dedicated tool and ship .dds or .ktx; this exists so
    // the demo can produce genuine compressed blocks rather than asserting the format works. It
    // is the simple endpoint-fitting approach — good enough to look right, nowhere near what a
    // production encoder achieves.
    //
    // BC3 stores each 4x4 block in 16 bytes: 8 for alpha (two endpoints plus 3-bit indices) and
    // 8 for colour (two RGB565 endpoints plus 2-bit indices).
    // ─────────────────────────────────────────────────────────────────────────

    uint16_t toRgb565(uint8_t r, uint8_t g, uint8_t b)
    {
        return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
    }

    /// @brief Encodes the 8 alpha bytes of one BC3 block.
    void encodeAlphaBlock(const uint8_t alpha[16], uint8_t out[8])
    {
        uint8_t minA = 255, maxA = 0;
        for (int i = 0; i < 16; ++i) {
            minA = std::min(minA, alpha[i]);
            maxA = std::max(maxA, alpha[i]);
        }

        out[0] = maxA;
        out[1] = minA;

        // With max > min the block uses 8 interpolated values; indices 0 and 1 are the endpoints
        // and 2..7 walk between them.
        uint64_t indices = 0;
        const int range = maxA - minA;
        for (int i = 0; i < 16; ++i) {
            uint32_t index = 0;
            if (range > 0) {
                const int t = ((alpha[i] - minA) * 7 + range / 2) / range;
                // Interpolation order is not simply 0..7: 0 is max, 1 is min, then 2..7 descend.
                index = (t == 7) ? 0u : (t == 0 ? 1u : static_cast<uint32_t>(8 - t));
            }
            indices |= static_cast<uint64_t>(index & 0x7) << (3 * i);
        }
        for (int i = 0; i < 6; ++i) {
            out[2 + i] = static_cast<uint8_t>((indices >> (8 * i)) & 0xFF);
        }
    }

    /// @brief Encodes the 8 colour bytes of one BC1 or BC3 block.
    void encodeColorBlock(const uint8_t rgb[16][3], uint8_t out[8])
    {
        // Endpoints from the per-channel bounding box. A real encoder fits a line through the
        // block's colours instead, which is most of the quality difference.
        uint8_t lo[3] = { 255, 255, 255 };
        uint8_t hi[3] = { 0, 0, 0 };
        for (int i = 0; i < 16; ++i) {
            for (int c = 0; c < 3; ++c) {
                lo[c] = std::min(lo[c], rgb[i][c]);
                hi[c] = std::max(hi[c], rgb[i][c]);
            }
        }

        uint16_t c0 = toRgb565(hi[0], hi[1], hi[2]);
        uint16_t c1 = toRgb565(lo[0], lo[1], lo[2]);
        // c0 > c1 selects the four-colour (opaque) mode, which is what BC3 always uses.
        if (c0 < c1) std::swap(c0, c1);

        out[0] = static_cast<uint8_t>(c0 & 0xFF);
        out[1] = static_cast<uint8_t>(c0 >> 8);
        out[2] = static_cast<uint8_t>(c1 & 0xFF);
        out[3] = static_cast<uint8_t>(c1 >> 8);

        // Project each texel onto the endpoint axis and quantise to one of four steps.
        float axis[3];
        float axisLengthSq = 0.0f;
        for (int c = 0; c < 3; ++c) {
            axis[c] = static_cast<float>(hi[c]) - static_cast<float>(lo[c]);
            axisLengthSq += axis[c] * axis[c];
        }

        uint32_t indices = 0;
        for (int i = 0; i < 16; ++i) {
            uint32_t index = 0;
            if (axisLengthSq > 0.0f) {
                float dot = 0.0f;
                for (int c = 0; c < 3; ++c) {
                    dot += (static_cast<float>(rgb[i][c]) - static_cast<float>(lo[c])) * axis[c];
                }
                const float t = std::clamp(dot / axisLengthSq, 0.0f, 1.0f);
                const int step = static_cast<int>(t * 3.0f + 0.5f);
                // Index order for the four-colour mode: 0 = c0, 1 = c1, 2 and 3 interpolate.
                static const uint32_t stepToIndex[4] = { 1, 3, 2, 0 };
                index = stepToIndex[step];
            }
            indices |= (index & 0x3u) << (2 * i);
        }
        for (int i = 0; i < 4; ++i) {
            out[4 + i] = static_cast<uint8_t>((indices >> (8 * i)) & 0xFF);
        }
    }

    /// @brief Compresses tightly packed RGBA8 pixels into BC3 blocks.
    std::vector<uint8_t> compressBC3(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height)
    {
        const uint32_t blocksX = (width + 3) / 4;
        const uint32_t blocksY = (height + 3) / 4;
        std::vector<uint8_t> blocks(static_cast<size_t>(blocksX) * blocksY * 16);

        for (uint32_t by = 0; by < blocksY; ++by) {
            for (uint32_t bx = 0; bx < blocksX; ++bx) {
                uint8_t color[16][3];
                uint8_t alpha[16];

                for (int i = 0; i < 16; ++i) {
                    // Clamp at the edges so a texture whose size is not a multiple of four still
                    // produces well-defined blocks.
                    const uint32_t x = std::min(bx * 4 + (i % 4), width - 1);
                    const uint32_t y = std::min(by * 4 + (i / 4), height - 1);
                    const size_t texel = (static_cast<size_t>(y) * width + x) * 4;
                    color[i][0] = rgba[texel + 0];
                    color[i][1] = rgba[texel + 1];
                    color[i][2] = rgba[texel + 2];
                    alpha[i] = rgba[texel + 3];
                }

                uint8_t* block = &blocks[(static_cast<size_t>(by) * blocksX + bx) * 16];
                encodeAlphaBlock(alpha, block);
                encodeColorBlock(color, block + 8);
            }
        }
        return blocks;
    }

    /// @brief Halves an RGBA8 image with a box filter, for building a mip chain on the CPU.
    std::vector<uint8_t> downsampleHalf(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height)
    {
        const uint32_t halfWidth = std::max(1u, width / 2);
        const uint32_t halfHeight = std::max(1u, height / 2);
        std::vector<uint8_t> result(static_cast<size_t>(halfWidth) * halfHeight * 4);

        for (uint32_t y = 0; y < halfHeight; ++y) {
            for (uint32_t x = 0; x < halfWidth; ++x) {
                for (uint32_t c = 0; c < 4; ++c) {
                    uint32_t sum = 0;
                    for (uint32_t dy = 0; dy < 2; ++dy) {
                        for (uint32_t dx = 0; dx < 2; ++dx) {
                            const uint32_t sx = std::min(x * 2 + dx, width - 1);
                            const uint32_t sy = std::min(y * 2 + dy, height - 1);
                            sum += rgba[(static_cast<size_t>(sy) * width + sx) * 4 + c];
                        }
                    }
                    result[(static_cast<size_t>(y) * halfWidth + x) * 4 + c] =
                        static_cast<uint8_t>(sum / 4);
                }
            }
        }
        return result;
    }

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

    /// Depth format for the 3D pass. 32-bit float is universally supported as a depth attachment.
    constexpr ImageFormat kDepthFormat = ImageFormat::D32_FLOAT;

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
        // The single-target plane pass shares its render pass with the cube, so it must declare
        // the same depth format even though it is a background that neither tests nor writes
        // depth. Render pass compatibility is about attachments, not about what a shader uses.
        std::shared_ptr<Pipeline> planePipeline = helper::createPipeline(
            device, planeVertexFunction, planeFragmentFunction,
            RenderTargetFormat::singleTarget(kTargetFormat, kDepthFormat));

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
            RenderTargetFormat::singleTarget(kTargetFormat, kDepthFormat, msaaSamples));

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

        // The same overlay image, block compressed, with its whole mip chain compressed on the
        // CPU — a compressed texture cannot have its levels generated on the GPU.
        size_t uncompressedBytes = 0;
        size_t compressedBytes = 0;
        std::shared_ptr<GImage> overlayTextureBC;
        {
            ImageDesc bcDesc{};
            bcDesc.format = ImageFormat::BC3_UNORM;
            bcDesc.width = kOverlayTextureSize;
            bcDesc.height = kOverlayTextureSize;
            bcDesc.mipLevels = kFullMipChain;
            bcDesc.usage = ImageUsage::Sampled;
            bcDesc.debugName = "OverlayTextureBC3";
            overlayTextureBC = device->createImage(bcDesc);

            std::vector<uint8_t> levelPixels = overlayPixels;
            uint32_t levelWidth = kOverlayTextureSize;
            uint32_t levelHeight = kOverlayTextureSize;

            for (uint32_t level = 0; level < overlayTextureBC->mipLevels(); ++level) {
                const std::vector<uint8_t> blocks = compressBC3(levelPixels, levelWidth, levelHeight);
                overlayTextureBC->update(blocks.data(), blocks.size(), level);

                uncompressedBytes += levelPixels.size();
                compressedBytes += blocks.size();

                if (level + 1 < overlayTextureBC->mipLevels()) {
                    levelPixels = downsampleHalf(levelPixels, levelWidth, levelHeight);
                    levelWidth = std::max(1u, levelWidth / 2);
                    levelHeight = std::max(1u, levelHeight / 2);
                }
            }
        }

        // --- 3D scene resources ---

        // A volume texture: one image with depth, not a stack of 2D slices.
        const std::vector<uint8_t> volumePixels = makeVolumePixels(kVolumeSize);
        ImageDesc volumeDesc{};
        volumeDesc.type = ImageType::Image3D;
        volumeDesc.format = ImageFormat::RGBA8_UNORM;
        volumeDesc.width = kVolumeSize;
        volumeDesc.height = kVolumeSize;
        volumeDesc.depth = kVolumeSize;
        volumeDesc.usage = ImageUsage::Sampled | ImageUsage::TransferSrc;
        volumeDesc.debugName = "VolumeTexture";
        std::shared_ptr<GImage> volumeTexture = device->createImage(volumeDesc, volumePixels.data());

        // A cubemap: six array layers, uploaded a face at a time.
        ImageDesc cubeMapDesc{};
        cubeMapDesc.type = ImageType::CubeMap;
        cubeMapDesc.format = ImageFormat::RGBA8_UNORM;
        cubeMapDesc.width = kCubeFaceSize;
        cubeMapDesc.height = kCubeFaceSize;
        cubeMapDesc.usage = ImageUsage::Sampled | ImageUsage::TransferSrc;
        cubeMapDesc.debugName = "EnvironmentCubeMap";
        std::shared_ptr<GImage> environmentTexture = device->createImage(cubeMapDesc);
        for (uint32_t face = 0; face < 6; ++face) {
            const std::vector<uint8_t> facePixels = makeCubeFacePixels(kCubeFaceSize, face);
            environmentTexture->update(facePixels.data(), facePixels.size(), 0, face);
        }

        // Cube geometry: 8 corners, 12 triangles, wound so that back-face culling removes the
        // inside surfaces — the first time CullMode has had anything to do in this demo.
        const Vertex3D cubeVertices[] = {
            {{-0.5f, -0.5f, -0.5f}}, {{ 0.5f, -0.5f, -0.5f}},
            {{ 0.5f,  0.5f, -0.5f}}, {{-0.5f,  0.5f, -0.5f}},
            {{-0.5f, -0.5f,  0.5f}}, {{ 0.5f, -0.5f,  0.5f}},
            {{ 0.5f,  0.5f,  0.5f}}, {{-0.5f,  0.5f,  0.5f}},
        };
        const uint16_t cubeIndices[] = {
            0, 2, 1,  0, 3, 2,   // back
            4, 5, 6,  4, 6, 7,   // front
            0, 1, 5,  0, 5, 4,   // bottom
            3, 7, 6,  3, 6, 2,   // top
            0, 4, 7,  0, 7, 3,   // left
            1, 2, 6,  1, 6, 5,   // right
        };

        std::shared_ptr<GBuffer> cubeVertexBuffer = device->createBuffer(
            BufferType::Vertex, BufferUsage::Static, sizeof(cubeVertices), cubeVertices, "CubeVertexBuffer");
        std::shared_ptr<GBuffer> cubeIndexBuffer = device->createBuffer(
            BufferType::Index, BufferUsage::Static, sizeof(cubeIndices), cubeIndices, "CubeIndexBuffer");

        // Draw arguments the GPU reads for itself. Written once here; the point is that the CPU
        // does not restate the vertex or instance counts at draw time.
        DrawIndexedIndirectCommand cubeDrawCommand{};
        cubeDrawCommand.indexCount = sizeof(cubeIndices) / sizeof(uint16_t);
        cubeDrawCommand.instanceCount = kCubeInstances;
        cubeDrawCommand.firstIndex = 0;
        cubeDrawCommand.vertexOffset = 0;
        cubeDrawCommand.firstInstance = 0;
        std::shared_ptr<GBuffer> cubeIndirectBuffer = device->createBuffer(
            BufferType::Indirect, BufferUsage::Static, sizeof(cubeDrawCommand),
            &cubeDrawCommand, "CubeIndirectBuffer");

        // Per-instance offsets, indexed in the shader by the instance id. std140 gives a vec4
        // array a natural 16-byte stride, so no padding surprises here.
        std::array<float, kCubeInstances * 4> instanceOffsets{};
        for (uint32_t i = 0; i < kCubeInstances; ++i) {
            const int gridX = static_cast<int>(i % 3) - 1;
            const int gridY = static_cast<int>(i / 3) - 1;
            instanceOffsets[i * 4 + 0] = static_cast<float>(gridX) * 1.1f;
            instanceOffsets[i * 4 + 1] = static_cast<float>(gridY) * 1.1f;
            instanceOffsets[i * 4 + 2] = 0.0f;
            instanceOffsets[i * 4 + 3] = 0.0f;
        }
        std::shared_ptr<GBuffer> instanceBuffer = device->createBuffer(
            BufferType::Uniform, BufferUsage::Static, sizeof(instanceOffsets),
            instanceOffsets.data(), "CubeInstanceBuffer");

        const std::filesystem::path cubePath = std::filesystem::path(SHADER_DIR) / "Cube";
        std::shared_ptr<ShaderFunction> cubeVertexFunction =
            helper::createShaderFunction(device, cubePath, "cube_vertex_shader");
        std::shared_ptr<ShaderFunction> cubeFragmentFunction =
            helper::createShaderFunction(device, cubePath, "cube_fragment_shader");

        // The first pipeline in the project that actually tests depth and culls faces.
        auto makeCubePipeline = [&](SampleCount samples) {
            PipelineDesc desc{};
            desc.vertexFunction = cubeVertexFunction;
            desc.fragmentFunction = cubeFragmentFunction;
            desc.targetFormat = RenderTargetFormat::singleTarget(kTargetFormat, kDepthFormat, samples);
            desc.depthStencil = DepthStencilState::depthTestAndWrite();
            desc.rasterizer.cullMode = CullMode::Back;
            desc.rasterizer.frontFace = FrontFace::CounterClockwise;
            desc.debugName = "CubePipeline";
            return helper::createPipeline(device, desc);
        };
        std::shared_ptr<Pipeline> cubePipeline = makeCubePipeline(SampleCount::One);
        std::shared_ptr<Pipeline> cubePipelineMSAA = makeCubePipeline(msaaSamples);

        // Trilinear and clamped: the volume and the cubemap both want smooth interpolation, and
        // clamping stops a cubemap face bleeding into its neighbour at the seams.
        SamplerDesc sceneSamplerDesc{};
        std::shared_ptr<GSampler> sceneSampler = device->createSampler(sceneSamplerDesc, "SceneSampler");

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
        std::shared_ptr<GImage> depthTarget;
        std::shared_ptr<GImage> msaaDepthTarget;
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

            // Depth buffers for the 3D pass, one per sample count. Depth is never resolved —
            // nothing reads it back — so the multisample one simply lives alongside its colour.
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
        bool showCube = true;
        bool useIndirect = true;
        bool useCompressedOverlay = true;
        float volumeMix = 0.6f;
        float cubeAngle = 0.0f;
        double lastTime = glfwGetTime();

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();

            std::shared_ptr<GImage> nextImgToDraw = swapChain->acquireNextImage();
            if (!nextImgToDraw) { continue; }

            glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
            if (fbWidth == 0 || fbHeight == 0) { continue; }
            ensureTargets(fbWidth, fbHeight);

            const double now = glfwGetTime();
            cubeAngle += static_cast<float>(now - lastTime) * 0.6f;
            lastTime = now;

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
                    ImGui::Checkbox("BC3 compressed", &useCompressedOverlay);
                    ImGui::Text("%.0f KiB uncompressed -> %.0f KiB BC3 (%.1fx smaller)",
                                uncompressedBytes / 1024.0, compressedBytes / 1024.0,
                                static_cast<double>(uncompressedBytes) / compressedBytes);
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
                ImGui::Checkbox("3D cube (depth tested)", &showCube);
                if (showCube) {
                    if (useMultipleTargets) {
                        ImGui::TextUnformatted("Disabled while MRT is on: that pass has no depth");
                    } else {
                        ImGui::Text("volume %ux%ux%u | cubemap %ux%u x%u faces",
                                    volumeTexture->width(), volumeTexture->height(), volumeTexture->depth(),
                                    environmentTexture->width(), environmentTexture->height(),
                                    environmentTexture->arrayLayers());
                        ImGui::SliderFloat("Volume vs cubemap", &volumeMix, 0.0f, 1.0f);
                        ImGui::Checkbox("Indirect draw", &useIndirect);
                        ImGui::SameLine();
                        ImGui::Text("(%u instances, 1 command)", kCubeInstances);
                    }
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

                // Depth only exists on the single-target path, which is where the cube draws.
                // Cleared to 1.0: the far plane, so every fragment initially passes.
                if (!useMultipleTargets) {
                    planePass->setDepthStencilAttachment(
                        useMsaa ? msaaDepthTarget : depthTarget, true, 1.0f, false, 0);
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

                    // The 3D cube, drawn over the grid background with depth testing and back
                    // faces culled, sampling a volume texture and a cubemap.
                    if (showCube && !useMultipleTargets) {
                        const float aspect = (float)fbWidth / (float)fbHeight;
                        const Mat4 projection = perspective(1.0472f /* 60 deg */, aspect, 0.1f, 20.0f);
                        const Mat4 view = translation(0.0f, 0.0f, -2.5f);
                        const Mat4 model = multiply(rotationY(cubeAngle), rotationX(cubeAngle * 0.6f));

                        SceneConstants sceneConstants{};
                        const Mat4 mvp = multiply(projection, multiply(view, model));
                        std::copy(mvp.begin(), mvp.end(), sceneConstants.modelViewProjection);
                        sceneConstants.volumeMix = volumeMix;

                        buffer->setRenderPipeline(useMsaa ? cubePipelineMSAA : cubePipeline);
                        buffer->setVertexBuffer(0, cubeVertexBuffer);
                        buffer->setUniformBuffer(1, ShaderStage::Vertex, instanceBuffer);
                        buffer->setTexture(0, ShaderStage::Fragment, volumeTexture, sceneSampler);
                        buffer->setTexture(1, ShaderStage::Fragment, environmentTexture, sceneSampler);
                        buffer->setPushConstants(ShaderStage::Vertex, &sceneConstants, sizeof(sceneConstants));
                        buffer->setPushConstants(ShaderStage::Fragment, &sceneConstants, sizeof(sceneConstants));

                        if (useIndirect) {
                            // The counts live in the buffer, not in this call.
                            buffer->drawIndexedIndirect(cubeIndexBuffer, IndexType::UInt16,
                                                        cubeIndirectBuffer, 1);
                        } else {
                            buffer->drawIndexed(cubeIndexBuffer, IndexType::UInt16,
                                                sizeof(cubeIndices) / sizeof(uint16_t),
                                                kCubeInstances, 0, 0, 0);
                        }
                    }
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
                    buffer->setTexture(0, ShaderStage::Fragment,
                                       useCompressedOverlay ? overlayTextureBC : overlayTexture,
                                       overlaySampler);

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
