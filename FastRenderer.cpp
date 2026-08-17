// ───────────────────────────────────────────────────────────────────────────────
// FastRenderer.cpp — scene viewer
//
// Loads a scene (.obj / .stl / .ply) and walks around it. Built to handle the models in the
// McGuire Computer Graphics Archive — San Miguel above all, which is where every shortcut stops
// working: ten million triangles, 281 materials, 265 textures, and foliage whose masks live in
// the alpha channel of the albedo images rather than in the material file.
//
// The whole frame loop is written once against the abstractions in render_pipeline/ and runs on
// both backends without a single conditional-compilation directive.
//
// Why forward rendering rather than deferred: this scene has one dominant light, the sun. The
// deferred trade — a fixed 12 bytes per pixel of G-buffer traffic in exchange for turning
// (geometry x lights) into a sum — only repays itself from roughly five lights upward, and it
// costs MSAA, which single-pixel foliage badly needs. So: forward, multisampled, one sun.
// ───────────────────────────────────────────────────────────────────────────────
#include "FastRenderer.hpp"
#include <GLFW/glfw3.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <tuple>
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
#include "texture/TextureCache.hpp"

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"

namespace dmrender
{
    /// Colour target format. Deliberately not sRGB: ImGui writes colours that are already
    /// encoded, and an sRGB framebuffer would encode them a second time and wash the interface
    /// out. The scene shader therefore encodes its own output.
    constexpr ImageFormat kColorFormat = ImageFormat::BGRA8_UNORM;
    /// 32-bit float depth. Reverse-Z buys nothing without it.
    constexpr ImageFormat kDepthFormat = ImageFormat::D32_FLOAT;
    /// Shadow cascades store world-space distance; see the note where they are created.
    constexpr ImageFormat kShadowFormat = ImageFormat::R32_FLOAT;

    /// Buffer slots. Slot 0 is geometry by convention; 1 is per-pass state; 2 is instances.
    constexpr uint32_t kFrameSlot = 1;
    constexpr uint32_t kInstanceSlot = 2;
    /// Texture slot the cascade array is bound to. 0 is albedo, 1 is the normal map.
    constexpr uint32_t kShadowSlot = 2;

    // ─────────────────────────────────────────────────────────────────────────
    // Shader-facing structures. Each must match its counterpart byte for byte.
    // ─────────────────────────────────────────────────────────────────────────

    /// How many shadow cascades the sun is split into. Four is the usual compromise: enough
    /// that the nearest one is sharp without paying for a fifth full pass over the scene.
    constexpr uint32_t kCascadeCount = 4;

    /// 480 bytes. Every vec3 is followed by a float so each pair fills exactly one 16-byte slot,
    /// which is what makes the C++ and std140 layouts agree without explicit padding.
    struct FrameUniforms {
        float viewProjection[16];
        float cameraPosition[3];  float exposure;
        float sunDirection[3];    float sunIntensity;
        float sunColor[3];        float ambientIntensity;
        float skyColor[3];        float shadowSoftness;
        float groundColor[3];     float shadowNormalBias;
        float cameraForward[3];   float shadowConstantBias;
        float cascadeSplit[4];            ///< View depth at which each cascade ends.
        float cascadeTexelWorldSize[4];   ///< World units covered by one shadow texel.
        float cascadeDepthRange[4];       ///< World units spanned by each cascade's depth axis.
        float cascadeMatrix[kCascadeCount][16];
        float shadowStrength;
        float shadowEnabled;
        float cascadeDebug;
        float pad;
    };
    static_assert(sizeof(FrameUniforms) == 480, "must match FrameUniforms in the shaders");

    /// 80 bytes. One per cascade, so it belongs to the pass rather than to the draw.
    struct ShadowPassUniforms {
        float lightViewProjection[16];
        float depthRange;
        float pad[3];
    };
    static_assert(sizeof(ShadowPassUniforms) == 80,
                  "must match ShadowPassUniforms in the shadow shaders");

    /**
     * @brief Spacing between the cascades' entries in the shared uniform buffer.
     *
     * All four cascades live in one buffer and are selected by binding offset, because updating
     * a single dynamic buffer once per cascade does *not* work: a dynamic buffer has one region
     * per frame in flight, so four updates within a frame all land in the same region and every
     * cascade pass ends up reading whichever matrix was written last. The symptom is a scene
     * that darkens uniformly instead of growing shadows — which is exactly what it did.
     *
     * 256 bytes because drivers require uniform binding offsets to be a multiple of
     * minUniformBufferOffsetAlignment, and 256 is the largest value in common use.
     */
    constexpr size_t kShadowUniformStride = 256;

    /// 48 bytes of the guaranteed 128. Per-draw, so it rides inside the command stream.
    struct DrawConstants {
        float baseColor[4];   ///< rgb colour, a opacity
        float material[4];    ///< x roughness, y metallic, z alphaCutoff, w hasNormalMap
        float emissive[4];
    };
    static_assert(sizeof(DrawConstants) <= kMaxPushConstantBytes,
                  "DrawConstants must fit the guaranteed push constant range");

    /// 80 bytes. One per drawn copy; this viewer draws the scene once, but the mechanism is what
    /// the vertex shader is written against.
    struct InstanceData {
        float model[16];
        float tint[4];
    };
    static_assert(sizeof(InstanceData) == 80, "must match InstanceData in the shaders");

    // ─────────────────────────────────────────────────────────────────────────
    // Minimal maths. Application code: the abstraction knows nothing about matrices.
    // ─────────────────────────────────────────────────────────────────────────

    using Mat4 = std::array<float, 16>;   // column-major, as in GLSL and MSL

    struct Vec3 { float x = 0.0f, y = 0.0f, z = 0.0f; };

    inline Vec3 operator+(Vec3 a, Vec3 b) { return { a.x + b.x, a.y + b.y, a.z + b.z }; }
    inline Vec3 operator-(Vec3 a, Vec3 b) { return { a.x - b.x, a.y - b.y, a.z - b.z }; }
    inline Vec3 operator*(Vec3 a, float s) { return { a.x * s, a.y * s, a.z * s }; }
    inline Vec3& operator+=(Vec3& a, Vec3 b) { a = a + b; return a; }
    inline Vec3& operator-=(Vec3& a, Vec3 b) { a = a - b; return a; }
    inline float dot(Vec3 a, Vec3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
    inline Vec3 cross(Vec3 a, Vec3 b) {
        return { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    }
    inline float length(Vec3 v) { return std::sqrt(dot(v, v)); }
    inline Vec3 normalize(Vec3 v) {
        const float l = length(v);
        return l > 1e-20f ? v * (1.0f / l) : Vec3{ 0.0f, 1.0f, 0.0f };
    }

    Mat4 identity()
    {
        Mat4 m{};
        m[0] = m[5] = m[10] = m[15] = 1.0f;
        return m;
    }

    Mat4 multiply(const Mat4& a, const Mat4& b)
    {
        Mat4 result{};
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                float sum = 0.0f;
                for (int k = 0; k < 4; ++k) sum += a[k * 4 + row] * b[column * 4 + k];
                result[column * 4 + row] = sum;
            }
        }
        return result;
    }

    /**
     * @brief Right-handed perspective with a *reversed* depth range: near maps to 1, far to 0.
     *
     * Floating-point values are dense near zero; the hyperbolic depth distribution is dense near
     * the near plane. Reversing one against the other makes the two non-uniformities cancel, and
     * relative precision becomes nearly constant across the whole range. On a scene spanning
     * from a doorknob to the far end of a courtyard this is the difference between clean geometry
     * and a wall of z-fighting.
     *
     * It only works with a floating-point depth buffer, and it needs two matching changes
     * elsewhere: clear depth to 0 rather than 1, and compare with Greater rather than Less.
     */
    Mat4 perspectiveReverseZ(float fovYRadians, float aspect, float nearZ, float farZ)
    {
        const float f = 1.0f / std::tan(fovYRadians * 0.5f);
        Mat4 m{};
        m[0]  = f / aspect;
        m[5]  = f;
        m[10] = nearZ / (farZ - nearZ);
        m[11] = -1.0f;
        m[14] = (nearZ * farZ) / (farZ - nearZ);
        return m;
    }

    Mat4 lookAt(Vec3 eye, Vec3 target, Vec3 up)
    {
        const Vec3 f = normalize(target - eye);
        const Vec3 s = normalize(cross(f, up));
        const Vec3 u = cross(s, f);

        Mat4 m{};
        m[0] = s.x;  m[4] = s.y;  m[8]  = s.z;  m[12] = -dot(s, eye);
        m[1] = u.x;  m[5] = u.y;  m[9]  = u.z;  m[13] = -dot(u, eye);
        m[2] = -f.x; m[6] = -f.y; m[10] = -f.z; m[14] = dot(f, eye);
        m[15] = 1.0f;
        return m;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Frustum culling
    // ─────────────────────────────────────────────────────────────────────────

    struct Plane { float nx, ny, nz, d; };

    /**
     * @brief The six clip planes of @p viewProjection, in world space.
     *
     * Derived from the clip conditions themselves rather than from named planes, which is what
     * makes this correct under both ordinary and reversed depth. The conditions are always
     * `-w <= x <= w`, `-w <= y <= w`, `0 <= z <= w`; only the matrix producing them changes.
     * Hardcoding "near = row2 + row3" instead is the classic error, and it culls everything
     * inside the scene while keeping what is behind the camera.
     */
    std::array<Plane, 6> extractFrustumPlanes(const Mat4& m)
    {
        auto row = [&](int r) {
            return std::array<float, 4>{ m[0 * 4 + r], m[1 * 4 + r], m[2 * 4 + r], m[3 * 4 + r] };
        };
        const auto r0 = row(0), r1 = row(1), r2 = row(2), r3 = row(3);

        auto combine = [](const std::array<float, 4>& a, const std::array<float, 4>& b, float sign) {
            Plane p{ a[0] + sign * b[0], a[1] + sign * b[1],
                     a[2] + sign * b[2], a[3] + sign * b[3] };
            const float len = std::sqrt(p.nx * p.nx + p.ny * p.ny + p.nz * p.nz);
            if (len > 1e-9f) { p.nx /= len; p.ny /= len; p.nz /= len; p.d /= len; }
            else             { p.nx = p.ny = p.nz = 0.0f; p.d = 1.0f; }   // degenerate: never culls
            return p;
        };

        return {
            combine(r3, r0,  1.0f),   // w + x >= 0
            combine(r3, r0, -1.0f),   // w - x >= 0
            combine(r3, r1,  1.0f),   // w + y >= 0
            combine(r3, r1, -1.0f),   // w - y >= 0
            combine(r2, r2,  0.0f),   // z     >= 0
            combine(r3, r2, -1.0f),   // w - z >= 0
        };
    }

    bool insideFrustum(const std::array<Plane, 6>& planes,
                       const float boundsMin[3], const float boundsMax[3])
    {
        for (const Plane& p : planes) {
            // The corner furthest along the plane normal. If even that is outside, the whole box
            // is. Conservative: a box may survive whose contents would not, which is the
            // harmless direction to err in.
            const float px = p.nx >= 0.0f ? boundsMax[0] : boundsMin[0];
            const float py = p.ny >= 0.0f ? boundsMax[1] : boundsMin[1];
            const float pz = p.nz >= 0.0f ? boundsMax[2] : boundsMin[2];
            if (p.nx * px + p.ny * py + p.nz * pz + p.d < 0.0f) return false;
        }
        return true;
    }

    // Declared here rather than with the rest of the camera code below, because
    // computeCascades() fits its volumes to the camera frustum and needs the type.
    struct Camera {
        Vec3  position{ 0.0f, 1.6f, 3.0f };
        float yaw = 0.0f;      ///< Radians around Y.
        float pitch = 0.0f;    ///< Radians, clamped short of vertical.
        float fovY = 1.0472f;  ///< 60 degrees.
        float speed = 3.0f;    ///< Units per second.

        Vec3 forward() const {
            return { std::cos(pitch) * std::sin(yaw),
                     std::sin(pitch),
                    -std::cos(pitch) * std::cos(yaw) };
        }
        Vec3 right() const { return normalize(cross(forward(), Vec3{ 0.0f, 1.0f, 0.0f })); }
        Mat4 view() const { return lookAt(position, position + forward(), { 0.0f, 1.0f, 0.0f }); }
    };

    Mat4 orthographic(float halfWidth, float halfHeight, float nearZ, float farZ)
    {
        Mat4 m{};
        m[0]  =  1.0f / halfWidth;
        m[5]  =  1.0f / halfHeight;
        m[10] = -1.0f / (farZ - nearZ);      // depth range [0, 1]
        m[14] = -nearZ / (farZ - nearZ);
        m[15] =  1.0f;
        return m;
    }

    /// Everything the lighting pass needs to know about one cascade.
    struct Cascade {
        Mat4  viewProjection = {};
        float splitDepth = 0.0f;      ///< View depth at which this cascade stops.
        float texelWorldSize = 0.0f;  ///< World units one shadow texel covers.
        float depthRange = 0.0f;      ///< World units along the light axis.
    };

    /**
     * @brief Splits the view range into cascades and fits an orthographic volume to each.
     *
     * Two details here are what separate a usable shadow from a shimmering one.
     *
     * *A bounding sphere, not a bounding box.* The slice of the view frustum is fitted with a
     * sphere, whose radius depends only on the slice's shape — not on which way the camera
     * happens to be pointing. Fit a box instead and the volume grows and shrinks as the camera
     * turns, changing the texel size every frame and making every shadow edge crawl.
     *
     * *Snapping to the texel grid.* The volume's centre is rounded to a whole number of texels.
     * Without it, moving the camera slides the whole grid by a fraction of a texel, each edge
     * resamples differently every frame, and the result boils. With it the grid moves in whole
     * texels, so a point on the ground keeps landing in the same texel it did before.
     */
    std::vector<Cascade> computeCascades(const Camera& camera, float aspect,
                                         float nearZ, float shadowDistance,
                                         Vec3 sunDirection, float resolution,
                                         float depthPadding)
    {
        std::vector<Cascade> cascades(kCascadeCount);

        const Vec3 forward = camera.forward();
        const Vec3 right = camera.right();
        const Vec3 up = normalize(cross(right, forward));

        const float tanHalfV = std::tan(camera.fovY * 0.5f);
        const float tanHalfH = tanHalfV * aspect;

        float sliceNear = nearZ;
        for (uint32_t index = 0; index < kCascadeCount; ++index) {
            // Practical split scheme: mostly logarithmic, which matches how quickly a
            // perspective projection loses resolution, nudged towards uniform so the nearest
            // cascade is not vanishingly thin.
            const float t = static_cast<float>(index + 1) / static_cast<float>(kCascadeCount);
            const float logarithmic = nearZ * std::pow(shadowDistance / nearZ, t);
            const float uniform = nearZ + (shadowDistance - nearZ) * t;
            const float sliceFar = 0.85f * logarithmic + 0.15f * uniform;

            // The eight corners of this slice, in world space.
            Vec3 corners[8];
            int corner = 0;
            for (float distance : { sliceNear, sliceFar }) {
                const Vec3 centre = camera.position + forward * distance;
                const float halfH = tanHalfV * distance;
                const float halfW = tanHalfH * distance;
                for (float sy : { -1.0f, 1.0f }) {
                    for (float sx : { -1.0f, 1.0f }) {
                        corners[corner++] = centre + right * (halfW * sx) + up * (halfH * sy);
                    }
                }
            }

            Vec3 centre{};
            for (const Vec3& c : corners) centre += c;
            centre = centre * (1.0f / 8.0f);

            float radius = 0.0f;
            for (const Vec3& c : corners) radius = std::max(radius, length(c - centre));
            radius = std::ceil(radius * 16.0f) / 16.0f;   // stop tiny changes rewriting the size

            const float texelWorldSize = (radius * 2.0f) / resolution;

            // Snap the centre onto the texel grid, in the light's own frame.
            const Vec3 lightUp = std::abs(sunDirection.y) > 0.99f ? Vec3{ 0.0f, 0.0f, 1.0f }
                                                                  : Vec3{ 0.0f, 1.0f, 0.0f };
            const Vec3 lightRight = normalize(cross(sunDirection, lightUp));
            const Vec3 lightTrueUp = cross(lightRight, sunDirection);

            auto snap = [&](Vec3 axis) {
                const float projected = dot(centre, axis);
                return std::floor(projected / texelWorldSize) * texelWorldSize - projected;
            };
            centre += lightRight * snap(lightRight);
            centre += lightTrueUp * snap(lightTrueUp);

            // Pull the eye back far enough that geometry between the light and the slice — a
            // roof above a courtyard, say — is still inside the volume and can cast into it.
            //
            // `sunDirection` points *towards* the sun, the convention the shading code uses, so
            // the light's eye goes in that direction from the slice and looks back at it. Getting
            // this sign wrong puts the eye underground: the map still fills with plausible
            // distances, but they describe the scene viewed from below, and almost every surface
            // then tests as occluded. The symptom is a scene that goes uniformly dim rather than
            // growing shadows — hard to read as a sign error, easy to mistake for a bias problem.
            const float backOff = radius + depthPadding;
            const Vec3 eye = centre + sunDirection * backOff;
            const float depthRange = backOff + radius;

            Cascade& cascade = cascades[index];
            cascade.viewProjection = multiply(orthographic(radius, radius, 0.0f, depthRange),
                                              lookAt(eye, centre, lightUp));
            cascade.splitDepth = sliceFar;
            cascade.texelWorldSize = texelWorldSize;
            cascade.depthRange = depthRange;

            sliceNear = sliceFar;
        }
        return cascades;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Camera
    // ─────────────────────────────────────────────────────────────────────────

    void updateCamera(Camera& camera, GLFWwindow* window, float deltaTime)
    {
        const ImGuiIO& io = ImGui::GetIO();

        static double lastX = 0.0, lastY = 0.0;
        static bool dragging = false;

        double mouseX = 0.0, mouseY = 0.0;
        glfwGetCursorPos(window, &mouseX, &mouseY);

        const bool held = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS ||
                          glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;

        // Without this check, dragging a slider also spins the camera.
        if (held && !io.WantCaptureMouse) {
            if (!dragging) { lastX = mouseX; lastY = mouseY; dragging = true; }

            constexpr float sensitivity = 0.0025f;
            camera.yaw   += static_cast<float>(mouseX - lastX) * sensitivity;
            camera.pitch -= static_cast<float>(mouseY - lastY) * sensitivity;
            // Exactly +/-90 degrees makes forward() parallel to up, the cross product zero and
            // the view matrix degenerate. Stop just short.
            camera.pitch = std::clamp(camera.pitch, -1.553f, 1.553f);
        } else {
            dragging = false;
        }
        lastX = mouseX;
        lastY = mouseY;

        if (io.WantCaptureKeyboard) return;

        const bool fast = glfwGetKey(window, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS;
        const bool slow = glfwGetKey(window, GLFW_KEY_LEFT_CONTROL) == GLFW_PRESS;
        // Movement scales with time; rotation above does not. Mouse movement is already a
        // displacement, and multiplying it by deltaTime would tie sensitivity to frame rate.
        const float step = camera.speed * deltaTime * (fast ? 6.0f : (slow ? 0.15f : 1.0f));

        if (glfwGetKey(window, GLFW_KEY_W) == GLFW_PRESS) camera.position += camera.forward() * step;
        if (glfwGetKey(window, GLFW_KEY_S) == GLFW_PRESS) camera.position -= camera.forward() * step;
        if (glfwGetKey(window, GLFW_KEY_D) == GLFW_PRESS) camera.position += camera.right() * step;
        if (glfwGetKey(window, GLFW_KEY_A) == GLFW_PRESS) camera.position -= camera.right() * step;
        if (glfwGetKey(window, GLFW_KEY_E) == GLFW_PRESS) camera.position.y += step;
        if (glfwGetKey(window, GLFW_KEY_Q) == GLFW_PRESS) camera.position.y -= step;
    }

    // ─────────────────────────────────────────────────────────────────────────
    // Draw list
    // ─────────────────────────────────────────────────────────────────────────

    struct DrawItem {
        uint32_t subsetIndex = 0;
        int32_t  materialIndex = -1;
        MaterialBlendMode blendMode = MaterialBlendMode::Opaque;
        bool     twoSided = false;
        float    viewDepth = 0.0f;
    };

    struct FrameStats {
        uint32_t drawCalls = 0;
        uint32_t pipelineChanges = 0;
        uint32_t materialChanges = 0;
        uint32_t subsetsVisible = 0;
        uint32_t subsetsCulled = 0;
        uint64_t trianglesSubmitted = 0;
        uint32_t shadowDraws = 0;
        uint32_t shadowSkipped = 0;
        uint32_t shadowIndirectCalls = 0;
        uint64_t shadowTriangles = 0;
    };

    /// Identity of a pipeline variant. Built on demand for the combinations the scene's
    /// materials actually require, rather than all of them up front.
    struct PipelineKey {
        MaterialBlendMode blendMode;
        bool twoSided;
        bool multisampled;
        bool shadowPass = false;

        bool operator<(const PipelineKey& other) const {
            return std::tie(blendMode, twoSided, multisampled, shadowPass) <
                   std::tie(other.blendMode, other.twoSided, other.multisampled, other.shadowPass);
        }
    };

    void FramebufferSizeCallback(GLFWwindow* window, int width, int height)
    {
        auto* chain = static_cast<std::shared_ptr<SwapChain>*>(glfwGetWindowUserPointer(window));
        if (*chain) (*chain)->recreate(width, height);
    }

    /**
     * @brief Finds a model to open.
     *
     * Order: an explicit DMRENDER_MODEL environment variable, then the archive scenes if they
     * have been unpacked beside the project, then the bundled test model. Walking up a few
     * directories covers being launched from the build folder as well as from the source root.
     */
    std::filesystem::path resolveModelPath()
    {
        if (const char* fromEnvironment = std::getenv("DMRENDER_MODEL")) {
            std::filesystem::path path(fromEnvironment);
            if (std::filesystem::exists(path)) return path;
            std::fprintf(stderr, "DMRENDER_MODEL is set but does not exist: %s\n", fromEnvironment);
        }

        const char* candidates[] = {
            "assets/San_Miguel/san-miguel.obj",
            "assets/San_Miguel/san-miguel-low-poly.obj",
            "bunny.obj",
        };

        std::filesystem::path prefix;
        for (int level = 0; level <= 5; ++level) {
            for (const char* candidate : candidates) {
                const std::filesystem::path path = prefix / candidate;
                if (std::filesystem::exists(path)) return path;
            }
            prefix /= "..";
        }
        return "bunny.obj";
    }

    void run_loop()
    {
        using Clock = std::chrono::steady_clock;

        // ── Load the scene before opening a window: nothing to show without one ──
        const std::filesystem::path modelPath = resolveModelPath();
        std::fprintf(stderr, "Loading %s\n", modelPath.string().c_str());

        Mesh mesh;
        const auto loadStart = Clock::now();
        const bool fromCache = loadSceneCache(modelPath, mesh);

        if (!fromCache) {
            std::string loadError;
            mesh = loadMesh(modelPath, loadError);
            if (mesh.empty()) {
                std::fprintf(stderr, "Failed to load model: %s\n", loadError.c_str());
                return;
            }
            // Parsing a gigabyte of text OBJ is a minute of work; writing the result back turns
            // every subsequent start into three large reads.
            if (saveSceneCache(modelPath, mesh)) {
                std::fprintf(stderr, "Wrote cache: %s\n",
                             sceneCachePath(modelPath).filename().string().c_str());
            }
        }
        const double loadSeconds = std::chrono::duration<double>(Clock::now() - loadStart).count();

        std::fprintf(stderr,
                     "%s: %s, %zu vertices, %zu triangles, %zu subsets, %zu materials\n"
                     "  geometry %.0f MiB, loaded in %.2f s%s\n",
                     modelPath.filename().string().c_str(), mesh.sourceFormat.c_str(),
                     mesh.vertices.size(), mesh.indices.size() / 3,
                     mesh.subsets.size(), mesh.materials.size(),
                     mesh.geometryBytes() / 1048576.0, loadSeconds,
                     fromCache ? " (from cache)" : "");

        const Vec3 sceneCenter{ mesh.center()[0], mesh.center()[1], mesh.center()[2] };
        const float sceneExtent = std::max(mesh.boundsExtent(), 1e-3f);
        std::fprintf(stderr, "  bounds (%.2f %.2f %.2f) .. (%.2f %.2f %.2f), extent %.2f\n",
                     mesh.boundsMin[0], mesh.boundsMin[1], mesh.boundsMin[2],
                     mesh.boundsMax[0], mesh.boundsMax[1], mesh.boundsMax[2], sceneExtent);

        // ── Window and device ──
        if (!glfwInit()) return;
        glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
        GLFWwindow* window = glfwCreateWindow(1600, 900, "Scene Viewer", nullptr, nullptr);
        if (!window) { glfwTerminate(); return; }

        std::shared_ptr<Surface> surface = helper::createSurface(window, kColorFormat);
        std::shared_ptr<Device> device = helper::createDefaultDevice(surface);

        IMGUI_CHECKVERSION();
        ImGui::CreateContext();
        ImGui_ImplGlfw_InitForOther(window, true);

        std::shared_ptr<CommandQueue> queue = helper::createCommandQueue(device);

        int fbWidth = 0, fbHeight = 0;
        glfwGetFramebufferSize(window, &fbWidth, &fbHeight);
        std::shared_ptr<SwapChain> swapChain =
            helper::createSwapChain(device, queue, surface, fbWidth, fbHeight);
        glfwSetWindowUserPointer(window, &swapChain);
        glfwSetFramebufferSizeCallback(window, FramebufferSizeCallback);
        helper::initImgui(swapChain);

        // ── Textures ──
        // Decoding runs across every core; uploading stays here. A quarter of a gigabyte of PNG
        // takes long enough that doing it serially would be the slowest part of startup.
        const uint32_t maxTextureSize = 2048;
        TextureCache textures(device, maxTextureSize);

        const auto textureStart = Clock::now();
        {
            std::vector<std::filesystem::path> colorPaths, dataPaths;
            for (const MeshMaterial& material : mesh.materials) {
                if (!material.albedoTexture.empty()) colorPaths.push_back(material.albedoTexture);
                if (!material.alphaTexture.empty())  dataPaths.push_back(material.alphaTexture);
                if (!material.normalTexture.empty()) dataPaths.push_back(material.normalTexture);
            }
            // Colour goes into an sRGB format so the hardware decodes on read, before filtering.
            // Normal and mask data are linear and must not be touched.
            textures.preload(colorPaths, /*srgb=*/true);
            textures.preload(dataPaths, /*srgb=*/false);
        }
        const double textureSeconds =
            std::chrono::duration<double>(Clock::now() - textureStart).count();

        std::fprintf(stderr, "Textures: %zu loaded, %.0f MiB, %.2f s",
                     textures.count(), textures.uploadedBytes() / 1048576.0, textureSeconds);
        if (textures.missingCount() > 0) {
            std::fprintf(stderr, ", %zu MISSING (drawn magenta)", textures.missingCount());
        }
        std::fprintf(stderr, "\n");
        for (size_t i = 0; i < textures.missing().size() && i < 8; ++i) {
            std::fprintf(stderr, "  missing: %s\n", textures.missing()[i].c_str());
        }

        // ── Refine the material classification now that the pixels are known ──
        //
        // MTL can only declare a mask through `map_d`, and San Miguel's file does not use it at
        // all: its foliage carries the mask in the alpha channel of the albedo image. The file
        // says nothing, so the pixels have to. This is the one classification that cannot be made
        // at parse time, and getting it wrong draws every leaf as an opaque rectangle.
        uint32_t promotedToCutout = 0;
        for (MeshMaterial& material : mesh.materials) {
            if (material.blendMode == MaterialBlendMode::Opaque &&
                textures.hasVaryingAlpha(material.albedoTexture)) {
                material.blendMode = MaterialBlendMode::Cutout;
                material.twoSided = true;
                ++promotedToCutout;
            }
        }
        if (promotedToCutout > 0) {
            std::fprintf(stderr,
                         "Materials: %u promoted to alpha-cutout by inspecting texture alpha\n",
                         promotedToCutout);
        }

        // ── Geometry in video memory ──
        std::shared_ptr<GBuffer> vertexBuffer = device->createBuffer(
            BufferType::Vertex, BufferUsage::Static,
            mesh.vertices.size() * sizeof(MeshVertex), mesh.vertices.data(), "SceneVertices");
        std::shared_ptr<GBuffer> indexBuffer = device->createBuffer(
            BufferType::Index, BufferUsage::Static,
            mesh.indices.size() * sizeof(uint32_t), mesh.indices.data(), "SceneIndices");
        if (!vertexBuffer || !indexBuffer) {
            std::fprintf(stderr, "Failed to create geometry buffers (out of memory?)\n");
            return;
        }

        // One instance, identity transform: the scene is already in world space. The mechanism
        // stays because the vertex shader is written against it, and it costs 80 bytes.
        InstanceData instance{};
        const Mat4 modelMatrix = identity();
        std::copy(modelMatrix.begin(), modelMatrix.end(), instance.model);
        instance.tint[0] = instance.tint[1] = instance.tint[2] = instance.tint[3] = 1.0f;
        std::shared_ptr<GBuffer> instanceBuffer = device->createBuffer(
            BufferType::Storage, BufferUsage::Static,
            sizeof(InstanceData), &instance, "SceneInstances");

        std::shared_ptr<GBuffer> frameBuffer = device->createBuffer(
            BufferType::Uniform, BufferUsage::Dynamic,
            sizeof(FrameUniforms), nullptr, "FrameUniforms");

        // ── Sampler ──
        const uint32_t maxAnisotropy = device->maxSupportedAnisotropy();
        int anisotropy = static_cast<int>(std::min(maxAnisotropy, 8u));
        std::shared_ptr<GSampler> sampler;
        auto rebuildSampler = [&](uint32_t requested) {
            SamplerDesc desc{};
            desc.minFilter = SamplerFilter::Linear;
            desc.magFilter = SamplerFilter::Linear;
            desc.mipFilter = SamplerFilter::Linear;
            desc.addressU = SamplerAddressMode::Repeat;
            desc.addressV = SamplerAddressMode::Repeat;
            desc.maxAnisotropy = requested;
            sampler = device->createSampler(desc, "SceneSampler");
        };
        rebuildSampler(static_cast<uint32_t>(anisotropy));

        // ── Shaders ──
        const std::filesystem::path shaderPath = std::filesystem::path(SHADER_DIR) / "Mesh";
        std::shared_ptr<ShaderFunction> vertexFunction =
            helper::createShaderFunction(device, shaderPath, "mesh_vertex_shader");
        std::shared_ptr<ShaderFunction> fragmentFunction =
            helper::createShaderFunction(device, shaderPath, "mesh_fragment_shader");
        std::shared_ptr<ShaderFunction> maskedFunction =
            helper::createShaderFunction(device, shaderPath, "mesh_fragment_masked");
        if (!vertexFunction || !fragmentFunction || !maskedFunction) {
            std::fprintf(stderr, "Failed to load shaders from %s\n", shaderPath.string().c_str());
            return;
        }

        const std::filesystem::path shadowShaderPath =
            std::filesystem::path(SHADER_DIR) / "MeshShadow";
        std::shared_ptr<ShaderFunction> shadowVertexFunction =
            helper::createShaderFunction(device, shadowShaderPath, "shadow_vertex_shader");
        std::shared_ptr<ShaderFunction> shadowFragmentFunction =
            helper::createShaderFunction(device, shadowShaderPath, "shadow_fragment_shader");
        std::shared_ptr<ShaderFunction> shadowMaskedFunction =
            helper::createShaderFunction(device, shadowShaderPath, "shadow_fragment_masked");
        if (!shadowVertexFunction || !shadowFragmentFunction || !shadowMaskedFunction) {
            std::fprintf(stderr, "Failed to load shadow shaders from %s\n",
                         shadowShaderPath.string().c_str());
            return;
        }

        // Shadow settings. Resolution is fixed at startup because the cascade array is
        // allocated once; everything else is live.
        const uint32_t shadowResolution = 2048;
        // DMRENDER_NOSHADOW=1 starts with shadows off, for isolating their cost or their bugs.
        bool  shadowsEnabled = std::getenv("DMRENDER_NOSHADOW") == nullptr;
        bool  cascadeDebug = false;
        float shadowStrength = 1.0f;
        /**
         * @brief How far shadows reach, as a fraction of the scene's extent.
         *
         * 0.35 was too short for a courtyard: the far wall of San Miguel sits about 30 units from
         * the middle, the cascades stopped at 24, and everything past that faded to fully lit —
         * so the tree cast crisp shadows on the ground beside it and none at all on the wall
         * opposite. The cost of reaching further is resolution: the last cascade grows, its texels
         * grow with it, and fine shadows soften. At 0.6 the far cascade still resolves a leaf
         * across several texels.
         */
        float shadowDistanceFraction = 0.6f;
        float shadowNormalBias = 1.6f;          ///< In shadow texels.
        float shadowConstantBiasTexels = 0.9f;  ///< Also in texels, converted per cascade.
        float shadowSoftness = 1.0f;            ///< PCF tap spacing, in texels.

        const SampleCount maxSamples = device->maxSupportedSampleCount();
        const SampleCount msaaSamples =
            static_cast<uint32_t>(maxSamples) >= 4 ? SampleCount::Four : maxSamples;

        std::map<PipelineKey, std::shared_ptr<Pipeline>> pipelines;
        auto pipelineFor = [&](const PipelineKey& key) -> std::shared_ptr<Pipeline> {
            auto it = pipelines.find(key);
            if (it != pipelines.end()) return it->second;

            PipelineDesc desc{};

            if (key.shadowPass) {
                desc.vertexFunction = shadowVertexFunction;
                desc.fragmentFunction = (key.blendMode == MaterialBlendMode::Cutout)
                    ? shadowMaskedFunction : shadowFragmentFunction;
                desc.targetFormat =
                    RenderTargetFormat::singleTarget(kShadowFormat, kDepthFormat);

                // Ordinary depth here, not reversed: an orthographic projection distributes
                // depth linearly, so there is no non-uniformity for reverse-Z to cancel.
                desc.depthStencil.depthTestEnabled = true;
                desc.depthStencil.depthWriteEnabled = true;
                desc.depthStencil.depthCompareOp = CompareOp::Less;

                // Nothing is culled. Much of this scene is single-sided — tablecloths, leaves,
                // awnings — and culling by winding would leave holes in their shadows.
                desc.rasterizer.cullMode = CullMode::None;

                desc.bufferSlots = defaultBufferSlotLayout();
                desc.bufferSlots[kInstanceSlot] = BufferBindingType::Storage;
                desc.debugName = "ShadowPipeline";

                std::shared_ptr<Pipeline> shadowPipeline = helper::createPipeline(device, desc);
                pipelines.emplace(key, shadowPipeline);
                return shadowPipeline;
            }

            desc.vertexFunction = vertexFunction;
            desc.fragmentFunction = (key.blendMode == MaterialBlendMode::Cutout)
                ? maskedFunction : fragmentFunction;
            desc.targetFormat = RenderTargetFormat::singleTarget(
                kColorFormat, kDepthFormat,
                key.multisampled ? msaaSamples : SampleCount::One);

            desc.depthStencil.depthTestEnabled = true;
            // Reverse-Z: nearer fragments carry the *greater* value.
            desc.depthStencil.depthCompareOp = CompareOp::Greater;

            if (key.blendMode == MaterialBlendMode::Transparent) {
                desc.blendStates = { BlendState::alphaBlend() };
                // Test against the opaque depth but do not write it: a transparent surface must
                // not hide the transparent surface behind it.
                desc.depthStencil.depthWriteEnabled = false;
                desc.depthStencil.depthCompareOp = CompareOp::GreaterOrEqual;
            } else {
                desc.depthStencil.depthWriteEnabled = true;
            }

            desc.rasterizer.cullMode = key.twoSided ? CullMode::None : CullMode::Back;
            desc.rasterizer.frontFace = FrontFace::CounterClockwise;

            desc.bufferSlots = defaultBufferSlotLayout();
            desc.bufferSlots[kInstanceSlot] = BufferBindingType::Storage;
            desc.debugName = "ScenePipeline";

            std::shared_ptr<Pipeline> created = helper::createPipeline(device, desc);
            pipelines.emplace(key, created);
            return created;
        };

        // Build every variant the scene needs up front, so no frame pays for a compile.
        {
            const auto buildStart = Clock::now();
            for (bool multisampled : { false, true }) {
                if (multisampled && msaaSamples == SampleCount::One) continue;
                pipelineFor({ MaterialBlendMode::Opaque, false, multisampled, false });
                for (const MeshMaterial& material : mesh.materials) {
                    pipelineFor({ material.blendMode, material.twoSided, multisampled, false });
                }
            }
            // Two shadow variants: plain, and alpha-tested for foliage.
            pipelineFor({ MaterialBlendMode::Opaque, false, false, true });
            pipelineFor({ MaterialBlendMode::Cutout, false, false, true });
            std::fprintf(stderr, "Pipelines: %zu variants in %.2f s\n", pipelines.size(),
                         std::chrono::duration<double>(Clock::now() - buildStart).count());
        }

        // ── Shadow maps ──
        //
        // A texture array, one layer per cascade, holding *distance in world units* rather than
        // depth. Two things force that: this wrapper rests depth images in an attachment layout
        // that cannot be sampled, and SamplerDesc has no comparison mode. Storing distance means
        // the comparison happens by hand, and in exchange the bias constants below are in
        // centimetres instead of arbitrary buffer units.
        //
        // R32_FLOAT rather than R16_FLOAT: a half has about three significant digits, so at the
        // hundred-metre distances this volume spans its steps are around five centimetres —
        // coarse enough to lose the contact between a chair leg and the floor.
        ImageDesc cascadeDesc{};
        cascadeDesc.format = kShadowFormat;
        cascadeDesc.width = shadowResolution;
        cascadeDesc.height = shadowResolution;
        cascadeDesc.arrayLayers = kCascadeCount;
        cascadeDesc.usage = ImageUsage::ColorTarget | ImageUsage::Sampled |
                            ImageUsage::TransferSrc;
        cascadeDesc.debugName = "ShadowCascades";
        std::shared_ptr<GImage> shadowCascades = device->createImage(cascadeDesc);

        // One depth attachment, reused by every cascade pass and cleared each time. It exists
        // only to resolve which caster is nearest; it is never read.
        ImageDesc shadowDepthDesc{};
        shadowDepthDesc.format = kDepthFormat;
        shadowDepthDesc.width = shadowResolution;
        shadowDepthDesc.height = shadowResolution;
        shadowDepthDesc.usage = ImageUsage::DepthStencil;
        shadowDepthDesc.debugName = "ShadowDepth";
        std::shared_ptr<GImage> shadowDepth = device->createImage(shadowDepthDesc);

        // One entry per cascade, written together once a frame and selected by binding offset.
        std::vector<uint8_t> shadowUniformStaging(kShadowUniformStride * kCascadeCount, 0);
        std::shared_ptr<GBuffer> shadowUniforms = device->createBuffer(
            BufferType::Uniform, BufferUsage::Dynamic,
            shadowUniformStaging.size(), nullptr, "ShadowPassUniforms");

        if (!shadowCascades || !shadowDepth || !shadowUniforms) {
            std::fprintf(stderr, "Failed to create shadow resources\n");
            return;
        }

        // Nearest, and not negotiable. Linear filtering would average the stored distances, and
        // the mean of two distances either side of a silhouette is a value no surface has.
        SamplerDesc shadowSamplerDesc{};
        shadowSamplerDesc.minFilter = SamplerFilter::Nearest;
        shadowSamplerDesc.magFilter = SamplerFilter::Nearest;
        shadowSamplerDesc.mipFilter = SamplerFilter::Nearest;
        shadowSamplerDesc.addressU = SamplerAddressMode::ClampToEdge;
        shadowSamplerDesc.addressV = SamplerAddressMode::ClampToEdge;
        shadowSamplerDesc.addressW = SamplerAddressMode::ClampToEdge;
        std::shared_ptr<GSampler> shadowSampler =
            device->createSampler(shadowSamplerDesc, "ShadowSampler");

        /**
         * @brief Per-cascade caster lists, rebuilt each frame.
         *
         * Casters without an alpha mask need no per-draw state at all — same pipeline, same
         * buffers, same push constants — so the whole list can go to the GPU as one indirect
         * call instead of a thousand `drawIndexed`. That matters here because the shadow pass is
         * bound by the cost of *issuing* draws rather than by triangles: four cascades produce
         * around three thousand calls, and at a few microseconds each that is most of the frame.
         *
         * Masked casters still go one at a time. Each needs its own albedo texture bound, and an
         * indirect batch shares one binding across every command in it.
         */
        struct ShadowList {
            std::vector<DrawIndexedIndirectCommand> opaque;
            std::vector<uint32_t> maskedSubsets;
        };
        std::vector<ShadowList> shadowLists(kCascadeCount);
        /// Whether every cascade layer has been rendered into at least once.
        bool shadowMapsInitialised = false;

        // All four cascades' commands in one buffer, written once a frame and selected by
        // offset — the same reason the pass uniforms are laid out that way.
        const size_t shadowCommandStride =
            mesh.subsets.size() * sizeof(DrawIndexedIndirectCommand);
        std::vector<DrawIndexedIndirectCommand> shadowCommandStaging(
            mesh.subsets.size() * kCascadeCount);
        std::shared_ptr<GBuffer> shadowCommands = device->createBuffer(
            BufferType::Indirect, BufferUsage::Dynamic,
            shadowCommandStaging.size() * sizeof(DrawIndexedIndirectCommand),
            nullptr, "ShadowDrawCommands");

        std::fprintf(stderr, "Shadows: %u cascades at %ux%u, %.0f MiB\n",
                     kCascadeCount, shadowResolution, shadowResolution,
                     (static_cast<double>(shadowResolution) * shadowResolution * 4 *
                      kCascadeCount) / 1048576.0);

        // ── Render targets, rebuilt when the window changes size ──
        std::shared_ptr<GImage> msaaColorTarget;
        std::shared_ptr<GImage> depthTarget;
        std::shared_ptr<GImage> msaaDepthTarget;
        int targetWidth = 0, targetHeight = 0;

        auto ensureTargets = [&](int width, int height) {
            if (width == targetWidth && height == targetHeight && depthTarget) return;

            if (msaaSamples != SampleCount::One) {
                ImageDesc colorDesc{};
                colorDesc.format = kColorFormat;
                colorDesc.width = static_cast<uint32_t>(width);
                colorDesc.height = static_cast<uint32_t>(height);
                colorDesc.sampleCount = msaaSamples;
                colorDesc.usage = ImageUsage::ColorTarget;
                colorDesc.debugName = "MsaaColorTarget";
                msaaColorTarget = device->createImage(colorDesc);
            }

            ImageDesc depthDesc{};
            depthDesc.format = kDepthFormat;
            depthDesc.width = static_cast<uint32_t>(width);
            depthDesc.height = static_cast<uint32_t>(height);
            depthDesc.usage = ImageUsage::DepthStencil;
            depthDesc.debugName = "DepthTarget";
            depthTarget = device->createImage(depthDesc);

            if (msaaSamples != SampleCount::One) {
                depthDesc.sampleCount = msaaSamples;
                depthDesc.debugName = "MsaaDepthTarget";
                msaaDepthTarget = device->createImage(depthDesc);
            }

            targetWidth = width;
            targetHeight = height;
        };

        {
            const MemoryBudget budget = device->queryMemoryBudget();
            std::fprintf(stderr,
                         "VRAM after load: %.0f / %.0f MiB (%s), allocations %u\n"
                         "  vertex buffer in %s, index buffer in %s\n",
                         budget.deviceLocalUsedBytes / 1048576.0,
                         budget.deviceLocalBudgetBytes / 1048576.0,
                         budget.preciseBudget ? "measured" : "estimated",
                         budget.nativeAllocationCount,
                         vertexBuffer->memoryLocation() == MemoryLocation::DeviceLocal
                             ? "VRAM" : "host memory",
                         indexBuffer->memoryLocation() == MemoryLocation::DeviceLocal
                             ? "VRAM" : "host memory");
        }

        // ── Scene-dependent defaults ──
        Camera camera;
        auto resetCamera = [&]() {
            camera.position = sceneCenter + Vec3{ 0.0f, sceneExtent * 0.03f, sceneExtent * 0.30f };
            camera.yaw = 0.0f;
            camera.pitch = -0.05f;
        };
        resetCamera();
        camera.speed = sceneExtent * 0.15f;

        // Near matters far more than far for depth precision, so it is set generously and the
        // reverse-Z distribution absorbs the rest.
        float nearZ = sceneExtent * 0.0008f;
        float farZ  = sceneExtent * 4.0f;

        bool  useMsaa = msaaSamples != SampleCount::One;
        bool  cullingEnabled = true;
        bool  sortingEnabled = true;
        float exposure = 1.15f;
        float sunAzimuth = -0.6f;
        float sunElevation = 0.85f;
        float sunIntensity = 4.5f;
        float ambientIntensity = 0.75f;
        float alphaCutoff = 0.5f;
        float sunColor[3] = { 1.0f, 0.96f, 0.88f };
        float skyColor[3] = { 0.32f, 0.42f, 0.58f };
        float groundColor[3] = { 0.20f, 0.17f, 0.13f };
        ClearValue clearColor{};
        clearColor.color[0] = 0.42f; clearColor.color[1] = 0.55f;
        clearColor.color[2] = 0.75f; clearColor.color[3] = 1.0f;
        clearColor.depth = 0.0f;   // reverse-Z: "infinitely far" is zero

        // The sun direction is needed both to fit the cascades and to shade, so it is derived
        // in one place rather than recomputed from the angles twice.
        auto currentSunDirection = [&]() {
            return normalize({ std::cos(sunElevation) * std::sin(sunAzimuth),
                               std::sin(sunElevation),
                               std::cos(sunElevation) * std::cos(sunAzimuth) });
        };

        std::vector<DrawItem> drawItems;
        drawItems.reserve(mesh.subsets.size());
        FrameStats stats;

        // Culling, sorting and recording, factored out so the offscreen capture path below and
        // the interactive loop cannot drift apart — a screenshot that does not match what the
        // window shows would be worse than no screenshot at all.
        auto buildDrawList = [&](const Mat4& viewProjection, const Vec3& eye) {
            drawItems.clear();
            const std::array<Plane, 6> planes = extractFrustumPlanes(viewProjection);

            for (uint32_t i = 0; i < mesh.subsets.size(); ++i) {
                const MeshSubset& subset = mesh.subsets[i];
                if (subset.indexCount == 0) continue;

                if (cullingEnabled && !insideFrustum(planes, subset.boundsMin, subset.boundsMax)) {
                    ++stats.subsetsCulled;
                    continue;
                }

                DrawItem item;
                item.subsetIndex = i;
                item.materialIndex = subset.materialIndex;
                if (subset.materialIndex >= 0 &&
                    subset.materialIndex < static_cast<int32_t>(mesh.materials.size())) {
                    const MeshMaterial& material = mesh.materials[subset.materialIndex];
                    item.blendMode = material.blendMode;
                    item.twoSided = material.twoSided;
                }

                const std::array<float, 3> center = subset.center();
                item.viewDepth = length(Vec3{ center[0], center[1], center[2] } - eye);
                drawItems.push_back(item);
                ++stats.subsetsVisible;
            }

            if (!sortingEnabled) return;

            std::sort(drawItems.begin(), drawItems.end(),
                      [](const DrawItem& a, const DrawItem& b) {
                // Transparency last: it blends with whatever is already there, so everything it
                // should blend over must be drawn first. Not an optimisation — the picture is
                // simply wrong otherwise.
                const bool aTransparent = a.blendMode == MaterialBlendMode::Transparent;
                const bool bTransparent = b.blendMode == MaterialBlendMode::Transparent;
                if (aTransparent != bTransparent) return bTransparent;

                // Back to front: the only order in which "over" composites correctly.
                if (aTransparent) return a.viewDepth > b.viewDepth;

                // Opaque and cutout: group by pipeline, then by material, then front to back.
                // The first two minimise state changes; the third lets early-Z reject what is
                // hidden before its fragment shader runs.
                if (a.blendMode != b.blendMode) return a.blendMode < b.blendMode;
                if (a.twoSided != b.twoSided) return a.twoSided < b.twoSided;
                if (a.materialIndex != b.materialIndex) return a.materialIndex < b.materialIndex;
                return a.viewDepth < b.viewDepth;
            });
        };

        std::vector<Cascade> cascades(kCascadeCount);

        auto fillFrameUniforms = [&](const Mat4& viewProjection, const Vec3& eye,
                                     const Vec3& forward) {
            FrameUniforms frameUniforms{};
            std::copy(viewProjection.begin(), viewProjection.end(), frameUniforms.viewProjection);
            frameUniforms.cameraPosition[0] = eye.x;
            frameUniforms.cameraPosition[1] = eye.y;
            frameUniforms.cameraPosition[2] = eye.z;
            frameUniforms.exposure = exposure;

            const Vec3 sunDirection = currentSunDirection();
            frameUniforms.sunDirection[0] = sunDirection.x;
            frameUniforms.sunDirection[1] = sunDirection.y;
            frameUniforms.sunDirection[2] = sunDirection.z;
            frameUniforms.sunIntensity = sunIntensity;
            std::copy(std::begin(sunColor), std::end(sunColor), frameUniforms.sunColor);
            frameUniforms.ambientIntensity = ambientIntensity;
            std::copy(std::begin(skyColor), std::end(skyColor), frameUniforms.skyColor);
            std::copy(std::begin(groundColor), std::end(groundColor), frameUniforms.groundColor);

            frameUniforms.cameraForward[0] = forward.x;
            frameUniforms.cameraForward[1] = forward.y;
            frameUniforms.cameraForward[2] = forward.z;

            frameUniforms.shadowEnabled = shadowsEnabled ? 1.0f : 0.0f;
            frameUniforms.shadowStrength = shadowStrength;
            frameUniforms.cascadeDebug = cascadeDebug ? 1.0f : 0.0f;
            frameUniforms.shadowNormalBias = shadowNormalBias;
            frameUniforms.shadowSoftness = shadowSoftness;
            // The constant bias is authored in texels and converted using the *first* cascade's
            // texel size, so tightening the near cascade does not silently loosen the far ones.
            frameUniforms.shadowConstantBias =
                shadowConstantBiasTexels * cascades[0].texelWorldSize;

            for (uint32_t i = 0; i < kCascadeCount; ++i) {
                frameUniforms.cascadeSplit[i] = cascades[i].splitDepth;
                frameUniforms.cascadeTexelWorldSize[i] = cascades[i].texelWorldSize;
                frameUniforms.cascadeDepthRange[i] = cascades[i].depthRange;
                std::copy(cascades[i].viewProjection.begin(), cascades[i].viewProjection.end(),
                          frameUniforms.cascadeMatrix[i]);
            }

            frameBuffer->update(&frameUniforms, sizeof(frameUniforms));

            // Every cascade's pass uniforms in one write, for the reason documented on
            // kShadowUniformStride.
            for (uint32_t i = 0; i < kCascadeCount; ++i) {
                ShadowPassUniforms passUniforms{};
                std::copy(cascades[i].viewProjection.begin(), cascades[i].viewProjection.end(),
                          passUniforms.lightViewProjection);
                passUniforms.depthRange = cascades[i].depthRange;
                std::memcpy(shadowUniformStaging.data() + kShadowUniformStride * i,
                            &passUniforms, sizeof(passUniforms));
            }
            shadowUniforms->update(shadowUniformStaging.data(), shadowUniformStaging.size());
        };

        auto recordScene = [&](const std::shared_ptr<CommandBuffer>& cmd, bool multisampled) {
            Pipeline* boundPipeline = nullptr;
            int32_t boundMaterial = -2;

            auto bindShared = [&]() {
                cmd->setVertexBuffer(0, vertexBuffer);
                cmd->setStorageBuffer(kInstanceSlot, ShaderStage::Vertex, instanceBuffer);
                cmd->setUniformBuffer(kFrameSlot, ShaderStage::Vertex, frameBuffer);
                cmd->setUniformBuffer(kFrameSlot, ShaderStage::Fragment, frameBuffer);
            };
            bindShared();

            for (const DrawItem& item : drawItems) {
                const PipelineKey key{ item.blendMode, item.twoSided, multisampled };
                std::shared_ptr<Pipeline> pipeline = pipelineFor(key);
                if (!pipeline) continue;

                if (pipeline.get() != boundPipeline) {
                    cmd->setRenderPipeline(pipeline);
                    boundPipeline = pipeline.get();
                    ++stats.pipelineChanges;
                    // Changing pipeline changes the descriptor layout the bindings belong to, so
                    // "already bound" tracking has to start over. Skipping this reset gives a
                    // rare, sort-order-dependent bug where objects wear a neighbour's texture.
                    boundMaterial = -2;
                    bindShared();
                }

                const MeshMaterial* material =
                    (item.materialIndex >= 0 &&
                     item.materialIndex < static_cast<int32_t>(mesh.materials.size()))
                        ? &mesh.materials[item.materialIndex] : nullptr;

                if (item.materialIndex != boundMaterial) {
                    cmd->setTexture(0, ShaderStage::Fragment,
                                    material ? textures.get(material->albedoTexture, true)
                                             : textures.white(),
                                    sampler);
                    // Always bind slot 1, even with no normal map: a slot declared in the shader
                    // but left unbound is a validation error on one backend and undefined
                    // behaviour on the other. A 1x1 stand-in costs four bytes.
                    cmd->setTexture(1, ShaderStage::Fragment,
                                    (material && !material->normalTexture.empty())
                                        ? textures.get(material->normalTexture, false)
                                        : textures.flatNormal(),
                                    sampler);
                    cmd->setTexture(kShadowSlot, ShaderStage::Fragment,
                                    shadowCascades, shadowSampler);
                    boundMaterial = item.materialIndex;
                    ++stats.materialChanges;
                }

                DrawConstants constants{};
                if (material) {
                    constants.baseColor[0] = material->baseColor[0];
                    constants.baseColor[1] = material->baseColor[1];
                    constants.baseColor[2] = material->baseColor[2];
                    constants.baseColor[3] = material->opacity;
                    constants.material[0] = material->roughness;
                    constants.material[1] = material->metallic;
                    constants.material[3] = material->normalTexture.empty() ? 0.0f : 1.0f;
                    constants.emissive[0] = material->emissive[0];
                    constants.emissive[1] = material->emissive[1];
                    constants.emissive[2] = material->emissive[2];
                } else {
                    constants.baseColor[0] = 0.72f;
                    constants.baseColor[1] = 0.70f;
                    constants.baseColor[2] = 0.68f;
                    constants.baseColor[3] = 1.0f;
                    constants.material[0] = 0.6f;
                }
                constants.material[2] = alphaCutoff;

                cmd->setPushConstants(ShaderStage::Vertex, &constants, sizeof(constants));
                cmd->setPushConstants(ShaderStage::Fragment, &constants, sizeof(constants));

                const MeshSubset& subset = mesh.subsets[item.subsetIndex];
                cmd->drawIndexed(indexBuffer, IndexType::UInt32, subset.indexCount, 1,
                                 subset.firstIndex * sizeof(uint32_t), 0, 0);

                ++stats.drawCalls;
                stats.trianglesSubmitted += subset.indexCount / 3;
            }
        };

        /**
         * @brief Chooses what casts into each cascade and packs the indirect command lists.
         *
         * Run once a frame, before any recording, because the command buffer is dynamic: writing
         * it per cascade would put every cascade's commands in the same frame region and leave
         * all four passes reading the last one.
         */
        auto buildShadowLists = [&]() {
            for (uint32_t c = 0; c < kCascadeCount; ++c) {
                ShadowList& list = shadowLists[c];
                list.opaque.clear();
                list.maskedSubsets.clear();

                const Cascade& cascade = cascades[c];
                const std::array<Plane, 6> lightPlanes =
                    extractFrustumPlanes(cascade.viewProjection);

                for (uint32_t i = 0; i < mesh.subsets.size(); ++i) {
                    const MeshSubset& subset = mesh.subsets[i];
                    if (subset.indexCount == 0) continue;

                    const MeshMaterial* material =
                        (subset.materialIndex >= 0 &&
                         subset.materialIndex < static_cast<int32_t>(mesh.materials.size()))
                            ? &mesh.materials[subset.materialIndex] : nullptr;

                    // Glass casting a solid shadow looks worse than glass casting none.
                    if (material && material->blendMode == MaterialBlendMode::Transparent) continue;

                    if (!insideFrustum(lightPlanes, subset.boundsMin, subset.boundsMax)) continue;

                    // A caster smaller than the texel it would land in cannot produce a shadow
                    // anyone can see, but it costs a draw call to find that out. San Miguel is
                    // full of cutlery and crockery, and skipping them in the coarse cascades
                    // removes a large share of the pass for no visible change.
                    if (subset.radius() < cascade.texelWorldSize) { ++stats.shadowSkipped; continue; }

                    if (material && material->blendMode == MaterialBlendMode::Cutout) {
                        list.maskedSubsets.push_back(i);
                    } else {
                        DrawIndexedIndirectCommand command{};
                        command.indexCount = subset.indexCount;
                        command.instanceCount = 1;
                        // In indices here, unlike drawIndexed()'s byte offset. The two APIs
                        // disagree and each setter follows its own.
                        command.firstIndex = subset.firstIndex;
                        command.vertexOffset = 0;
                        command.firstInstance = 0;
                        list.opaque.push_back(command);
                    }

                    ++stats.shadowDraws;
                    stats.shadowTriangles += subset.indexCount / 3;
                }
            }

            for (uint32_t c = 0; c < kCascadeCount; ++c) {
                std::copy(shadowLists[c].opaque.begin(), shadowLists[c].opaque.end(),
                          shadowCommandStaging.begin() + mesh.subsets.size() * c);
            }
            shadowCommands->update(shadowCommandStaging.data(),
                                   shadowCommandStaging.size() *
                                       sizeof(DrawIndexedIndirectCommand));
        };

        /**
         * @brief Renders the casters for one cascade into its layer of the array.
         *
         * The draw list is rebuilt against the cascade's own volume rather than reused from the
         * camera: what casts into a cascade is not what is visible from the eye. Anything between
         * the sun and the slice matters, and the volume was extended towards the light for
         * exactly that reason.
         */
        auto recordShadowCascade = [&](const std::shared_ptr<CommandBuffer>& cmd,
                                       uint32_t cascadeIndex) {
            const Cascade& cascade = cascades[cascadeIndex];
            const ShadowList& list = shadowLists[cascadeIndex];
            // No early-out on an empty list. The pass is what transitions this layer into a
            // layout the lighting shader can sample, and its clear is what makes an empty
            // cascade read as "nothing in the way". Skipping it leaves the layer undefined,
            // which the validation layer catches and the hardware would read as garbage.

            std::shared_ptr<RenderPassDescriptor> pass = helper::createRenderPassDescriptor();
            ClearValue shadowClear{};
            // Cleared far beyond the volume, so anywhere no caster was drawn reads as "nothing
            // in the way". Clearing to zero instead puts the whole scene in shadow — the single
            // most common way to get this wrong.
            shadowClear.color[0] = cascade.depthRange * 4.0f;
            shadowClear.depth = 1.0f;   // ordinary depth in this pass, not reversed
            pass->setColorAttachment(0, shadowCascades, true, shadowClear, cascadeIndex);
            pass->setDepthStencilAttachment(shadowDepth, true, 1.0f, false, 0);

            cmd->beginRenderPass(pass);

            const size_t uniformOffset = kShadowUniformStride * cascadeIndex;
            auto bindShared = [&]() {
                cmd->setVertexBuffer(0, vertexBuffer);
                cmd->setStorageBuffer(kInstanceSlot, ShaderStage::Vertex, instanceBuffer);
                cmd->setUniformBuffer(kFrameSlot, ShaderStage::Vertex, shadowUniforms,
                                      uniformOffset);
                cmd->setUniformBuffer(kFrameSlot, ShaderStage::Fragment, shadowUniforms,
                                      uniformOffset);
            };

            // Everything without a mask, as a single call. The GPU reads the per-draw arguments
            // out of the buffer; the CPU never touches them again.
            if (!list.opaque.empty()) {
                cmd->setRenderPipeline(pipelineFor({ MaterialBlendMode::Opaque, false, false, true }));
                bindShared();
                cmd->drawIndexedIndirect(indexBuffer, IndexType::UInt32, shadowCommands,
                                         static_cast<uint32_t>(list.opaque.size()),
                                         shadowCommandStride * cascadeIndex);
                ++stats.shadowIndirectCalls;
            }

            // Masked casters one at a time: each needs its own albedo bound, and an indirect
            // batch shares one binding across every command in it.
            if (!list.maskedSubsets.empty()) {
                cmd->setRenderPipeline(pipelineFor({ MaterialBlendMode::Cutout, false, false, true }));
                bindShared();

                int32_t boundMaterial = -2;
                for (uint32_t subsetIndex : list.maskedSubsets) {
                    const MeshSubset& subset = mesh.subsets[subsetIndex];
                    const MeshMaterial& material = mesh.materials[subset.materialIndex];

                    if (subset.materialIndex != boundMaterial) {
                        cmd->setTexture(0, ShaderStage::Fragment,
                                        textures.get(material.albedoTexture, true), sampler);
                        boundMaterial = subset.materialIndex;

                        DrawConstants constants{};
                        constants.material[2] = alphaCutoff;
                        cmd->setPushConstants(ShaderStage::Fragment, &constants,
                                              sizeof(constants));
                    }

                    cmd->drawIndexed(indexBuffer, IndexType::UInt32, subset.indexCount, 1,
                                     subset.firstIndex * sizeof(uint32_t), 0, 0);
                }
            }

            cmd->endRenderPass();
        };

        /**
         * @brief Everything the shadow pass needs, in one call.
         *
         * Exists as a single function on purpose. Building the caster lists and recording the
         * cascade passes used to be separate calls made by each render path, and the interactive
         * path ended up making only the second of them: the cascades rendered, but with an empty
         * list, so every layer was cleared to "nothing in the way" and the window showed no
         * shadows at all while the offscreen capture — which did call both — showed them
         * correctly. Two paths that must agree should share one function, not two calls in the
         * right order.
         *
         * @pre cascades[] is current and fillFrameUniforms() has uploaded the per-cascade
         *      matrices for this frame.
         */
        auto renderShadows = [&](const std::shared_ptr<CommandBuffer>& cmd) {
            if (shadowsEnabled) {
                buildShadowLists();
            } else if (!shadowMapsInitialised) {
                // Shadows are off, but the lighting shader still declares the cascade array —
                // the branch is inside the shader, not around the binding — so the image needs a
                // defined layout. One clear-only round gives it that.
                for (ShadowList& list : shadowLists) {
                    list.opaque.clear();
                    list.maskedSubsets.clear();
                }
            } else {
                return;
            }

            for (uint32_t c = 0; c < kCascadeCount; ++c) recordShadowCascade(cmd, c);
            shadowMapsInitialised = true;
        };

        // ── Offscreen capture ──
        //
        // Set DMRENDER_SCREENSHOT to a path prefix to render a few fixed viewpoints to PNG and
        // exit, without opening an interactive session. This exists for two reasons: it is the
        // reference-image check the book asks for, and it exercises GImage::readback(), which
        // nothing else here does.
        //
        // The target is a private image rather than a swapchain image: swapchain images cannot
        // be read back, and a plain one can carry TransferSrc.
        if (const char* shotPrefix = std::getenv("DMRENDER_SCREENSHOT")) {
            const uint32_t shotWidth = 1600, shotHeight = 900;

            ImageDesc colorDesc{};
            colorDesc.format = kColorFormat;
            colorDesc.width = shotWidth;
            colorDesc.height = shotHeight;
            colorDesc.usage = ImageUsage::ColorTarget | ImageUsage::TransferSrc;
            colorDesc.debugName = "ScreenshotColor";
            std::shared_ptr<GImage> shotColor = device->createImage(colorDesc);

            // Capture through the same multisampled path the window uses. Rendering the stills
            // single-sampled would leave the pipeline variant the interactive loop actually binds
            // completely untested, which is the sort of gap that hides a bug for a long time.
            const bool shotMultisampled = useMsaa && msaaSamples != SampleCount::One;
            std::shared_ptr<GImage> shotMsaaColor;
            std::shared_ptr<GImage> shotMsaaDepth;
            if (shotMultisampled) {
                ImageDesc msaaColorDesc = colorDesc;
                msaaColorDesc.usage = ImageUsage::ColorTarget;
                msaaColorDesc.sampleCount = msaaSamples;
                msaaColorDesc.debugName = "ScreenshotMsaaColor";
                shotMsaaColor = device->createImage(msaaColorDesc);
            }

            ImageDesc shotDepthDesc{};
            shotDepthDesc.format = kDepthFormat;
            shotDepthDesc.width = shotWidth;
            shotDepthDesc.height = shotHeight;
            shotDepthDesc.usage = ImageUsage::DepthStencil;
            shotDepthDesc.debugName = "ScreenshotDepth";
            std::shared_ptr<GImage> shotDepth = device->createImage(shotDepthDesc);

            if (shotMultisampled) {
                shotDepthDesc.sampleCount = msaaSamples;
                shotDepthDesc.debugName = "ScreenshotMsaaDepth";
                shotMsaaDepth = device->createImage(shotDepthDesc);
            }

            if (!shotColor || !shotDepth ||
                (shotMultisampled && (!shotMsaaColor || !shotMsaaDepth))) {
                std::fprintf(stderr, "Screenshot: could not create targets\n");
                return;
            }

            // Poses as fractions of the bounding box, so the same sweep frames any scene.
            struct Pose { const char* name; float fx, fy, fz, yaw, pitch; };
            const Pose poses[] = {
                { "a", 0.50f, 0.10f, 0.85f,  0.00f, -0.05f },
                { "b", 0.50f, 0.10f, 0.50f,  0.00f, -0.05f },
                { "c", 0.50f, 0.10f, 0.50f,  1.57f, -0.05f },
                { "d", 0.50f, 0.10f, 0.50f,  3.14f, -0.05f },
                { "e", 0.50f, 0.10f, 0.50f, -1.57f, -0.05f },
                { "f", 0.50f, 0.45f, 0.90f,  0.00f, -0.45f },
            };

            std::vector<uint8_t> pixels(static_cast<size_t>(shotWidth) * shotHeight * 4);
            std::vector<uint8_t> rgba(pixels.size());

            // DMRENDER_BENCH=N also times N repeats of each viewpoint.
            int benchFrames = 0;
            if (const char* benchText = std::getenv("DMRENDER_BENCH")) {
                benchFrames = std::atoi(benchText);
            }

            for (const Pose& pose : poses) {
                Camera shotCamera = camera;
                shotCamera.position = {
                    mesh.boundsMin[0] + (mesh.boundsMax[0] - mesh.boundsMin[0]) * pose.fx,
                    mesh.boundsMin[1] + (mesh.boundsMax[1] - mesh.boundsMin[1]) * pose.fy,
                    mesh.boundsMin[2] + (mesh.boundsMax[2] - mesh.boundsMin[2]) * pose.fz,
                };
                shotCamera.yaw = pose.yaw;
                shotCamera.pitch = pose.pitch;

                const float shotAspect = static_cast<float>(shotWidth) / static_cast<float>(shotHeight);
                const Mat4 shotProjection =
                    perspectiveReverseZ(shotCamera.fovY, shotAspect, nearZ, farZ);
                const Mat4 shotViewProjection = multiply(shotProjection, shotCamera.view());

                stats = FrameStats{};
                cascades = computeCascades(shotCamera, shotAspect, nearZ,
                                           sceneExtent * shadowDistanceFraction,
                                           currentSunDirection(), float(shadowResolution),
                                           sceneExtent * 0.5f);
                buildDrawList(shotViewProjection, shotCamera.position);

                // The per-frame uploads belong *inside* the repeat, not before it.
                //
                // A BufferUsage::Dynamic buffer has one region per frame in flight, and update()
                // writes whichever region the frame being recorded owns. Writing once and then
                // rendering N frames leaves every other frame reading a region that was never
                // filled: with two frames in flight, an even repeat count ends on the untouched
                // one, whose zeroed view-projection matrix collapses the whole scene to a point
                // and yields a blank frame. An odd count ends on the good region and looks fine —
                // which is exactly the kind of intermittent result that wastes an afternoon.
                //
                // Uploading every frame is also the honest measurement: a real frame does this work.
                auto renderOnce = [&]() {
                    fillFrameUniforms(shotViewProjection, shotCamera.position,
                                      shotCamera.forward());

                    std::shared_ptr<CommandBuffer> cmd = helper::createCommandBuffer(queue);

                    renderShadows(cmd);

                    std::shared_ptr<RenderPassDescriptor> pass = helper::createRenderPassDescriptor();
                    if (shotMultisampled) {
                        pass->setColorAttachment(0, shotMsaaColor, true, clearColor);
                        pass->setResolveAttachment(0, shotColor);
                        pass->setDepthStencilAttachment(shotMsaaDepth, true, 0.0f, false, 0);
                    } else {
                        pass->setColorAttachment(0, shotColor, true, clearColor);
                        pass->setDepthStencilAttachment(shotDepth, true, 0.0f, false, 0);
                    }

                    cmd->beginRenderPass(pass);
                    recordScene(cmd, shotMultisampled);
                    cmd->endRenderPass();
                    cmd->commit();
                };

                renderOnce();
                // Snapshot before the timing loop: recordScene() accumulates into `stats`, so
                // after sixty repeats the counters would read sixty times the truth.
                const FrameStats shotStats = stats;

                // Throughput, measured with no swapchain in the way. In the interactive loop
                // acquireNextImage() blocks on the display, so wall time there reports the
                // refresh interval rather than the cost of the frame. Here nothing paces us, and
                // readback() waits for the GPU, which is what closes the timing window.
                double frameMilliseconds = 0.0;
                if (benchFrames > 0) {
                    const auto benchStart = Clock::now();
                    for (int i = 0; i < benchFrames; ++i) renderOnce();
                    shotColor->readback(pixels.data(), pixels.size());
                    frameMilliseconds =
                        std::chrono::duration<double, std::milli>(Clock::now() - benchStart).count()
                        / benchFrames;
                }

                shotColor->readback(pixels.data(), pixels.size());

                // The target is BGRA; PNG wants RGBA. Also force alpha opaque: the scene shader
                // writes coverage there, which a viewer would interpret as transparency.
                for (size_t i = 0; i < pixels.size(); i += 4) {
                    rgba[i + 0] = pixels[i + 2];
                    rgba[i + 1] = pixels[i + 1];
                    rgba[i + 2] = pixels[i + 0];
                    rgba[i + 3] = 255;
                }

                if (std::getenv("DMRENDER_DUMP_CASCADES") &&
                    std::string(pose.name) == "a" && shadowsEnabled) {
                    // Dump the cascades themselves. A shadow that looks wrong is almost always
                    // a shadow map that is wrong, and guessing from the lit result is slow.
                    std::vector<float> depths(static_cast<size_t>(shadowResolution) * shadowResolution);
                    std::vector<uint8_t> gray(depths.size());
                    for (uint32_t layer = 0; layer < kCascadeCount; ++layer) {
                        shadowCascades->readback(depths.data(), depths.size() * sizeof(float),
                                                 0, layer);
                        float lo = 1e30f, hi = -1e30f;
                        for (float d : depths) {
                            if (d < lo) lo = d;
                            if (d < 1e20f && d > hi) hi = d;
                        }
                        const float scale = (hi > lo) ? 1.0f / (hi - lo) : 1.0f;
                        for (size_t i = 0; i < depths.size(); ++i) {
                            const float t = std::clamp((depths[i] - lo) * scale, 0.0f, 1.0f);
                            gray[i] = static_cast<uint8_t>(t * 255.0f);
                        }
                        const std::string mapPath =
                            std::string(shotPrefix) + "_cascade" + std::to_string(layer) + ".png";
                        stbi_write_png(mapPath.c_str(), static_cast<int>(shadowResolution),
                                       static_cast<int>(shadowResolution), 1, gray.data(),
                                       static_cast<int>(shadowResolution));
                        std::fprintf(stderr, "  cascade %u: range %.3f .. %.3f -> %s\n",
                                     layer, lo, hi, mapPath.c_str());
                    }
                }

                const std::string outPath = std::string(shotPrefix) + "_" + pose.name + ".png";
                const int written = stbi_write_png(outPath.c_str(),
                                                   static_cast<int>(shotWidth),
                                                   static_cast<int>(shotHeight), 4,
                                                   rgba.data(),
                                                   static_cast<int>(shotWidth) * 4);
                std::fprintf(stderr,
                             "Screenshot %s: %s | %u draws, %u/%u subsets, %.2f M tris",
                             outPath.c_str(), written ? "ok" : "FAILED",
                             shotStats.drawCalls, shotStats.subsetsVisible,
                             shotStats.subsetsVisible + shotStats.subsetsCulled,
                             shotStats.trianglesSubmitted / 1e6);
                std::fprintf(stderr, " | shadow %u draws %.2f M tris",
                             shotStats.shadowDraws, shotStats.shadowTriangles / 1e6);
                if (benchFrames > 0) {
                    std::fprintf(stderr, " | %.2f ms/frame (%.0f FPS)",
                                 frameMilliseconds, 1000.0 / std::max(frameMilliseconds, 1e-6));
                }
                std::fprintf(stderr, "\n");
            }

            helper::shutdownImgui();
            ImGui_ImplGlfw_Shutdown();
            ImGui::DestroyContext();
            glfwDestroyWindow(window);
            glfwTerminate();
            return;
        }

        double lastTime = glfwGetTime();
        float smoothedDelta = 1.0f / 60.0f;

        // DMRENDER_FRAMES=N closes the window after N frames. Killing the process instead skips
        // shutdown entirely, which is exactly where a validation layer tends to have something
        // to say about objects still in use.
        int framesRemaining = 0;
        if (const char* framesText = std::getenv("DMRENDER_FRAMES")) {
            framesRemaining = std::atoi(framesText);
        }

        while (!glfwWindowShouldClose(window))
        {
            glfwPollEvents();

            if (framesRemaining > 0 && --framesRemaining == 0) {
                glfwSetWindowShouldClose(window, GLFW_TRUE);
            }

            // DMRENDER_RESIZE=1 resizes the window on a cycle. Resizing rebuilds the render
            // targets and invalidates cached framebuffers, which is the part of the frame a
            // headless run never touches and a person exercises within seconds.
            if (std::getenv("DMRENDER_RESIZE") && framesRemaining > 0 && (framesRemaining % 17) == 0) {
                const int widths[]  = { 1600, 1280, 900, 1440 };
                const int heights[] = { 900, 720, 640, 810 };
                const int pick = (framesRemaining / 17) % 4;
                glfwSetWindowSize(window, widths[pick], heights[pick]);
            }

            const double now = glfwGetTime();
            float rawDelta = static_cast<float>(now - lastTime);
            lastTime = now;
            // Discard outliers — the first frame, a debugger pause, a restore from minimised —
            // then smooth, so camera motion does not jitter with scheduling noise.
            if (rawDelta > 0.25f) rawDelta = smoothedDelta;
            smoothedDelta = smoothedDelta * 0.85f + rawDelta * 0.15f;

            std::shared_ptr<GImage> target = swapChain->acquireNextImage();
            if (!target) continue;

            // Sizes come from the swapchain, not the window: during a resize the two disagree for
            // a frame, and every attachment of a pass must match the swapchain image exactly.
            fbWidth = static_cast<int>(swapChain->width());
            fbHeight = static_cast<int>(swapChain->height());
            if (fbWidth == 0 || fbHeight == 0) continue;
            ensureTargets(fbWidth, fbHeight);

            updateCamera(camera, window, smoothedDelta);

            const float aspect = static_cast<float>(fbWidth) / static_cast<float>(fbHeight);
            const Mat4 projection = perspectiveReverseZ(camera.fovY, aspect, nearZ, farZ);
            const Mat4 viewProjection = multiply(projection, camera.view());

            // ── Shadow cascades, then cull and sort ──
            stats = FrameStats{};
            cascades = computeCascades(camera, aspect, nearZ,
                                       sceneExtent * shadowDistanceFraction,
                                       currentSunDirection(), float(shadowResolution),
                                       sceneExtent * 0.5f);
            buildDrawList(viewProjection, camera.position);

            // ── Per-pass uniforms ──
            fillFrameUniforms(viewProjection, camera.position, camera.forward());

            // ── Interface ──
            // A separate pass: ImGui built its pipeline against the swapchain configuration —
            // one sample, no depth — and cannot run inside a multisampled pass with depth.
            std::shared_ptr<RenderPassDescriptor> uiPass = helper::createRenderPassDescriptor();
            uiPass->setColorAttachment(0, target, /*clear=*/false, clearColor);

            helper::newFrameImgui(uiPass);
            ImGui_ImplGlfw_NewFrame();
            ImGui::NewFrame();
            {
                ImGui::SetNextWindowSize(ImVec2(380, 0), ImGuiCond_FirstUseEver);
                ImGui::Begin("Scene");

                ImGui::Text("%s", modelPath.filename().string().c_str());
                ImGui::Text("%s | %.2f M tris | %zu subsets",
                            mesh.sourceFormat.c_str(), mesh.indices.size() / 3.0 / 1e6,
                            mesh.subsets.size());
                ImGui::Text("%zu materials | %zu textures",
                            mesh.materials.size(), textures.count());
                if (textures.missingCount() > 0) {
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 1.0f, 1.0f), "%zu textures missing",
                                       textures.missingCount());
                }

                ImGui::Separator();
                ImGui::Text("%.1f FPS (%.2f ms)", ImGui::GetIO().Framerate,
                            1000.0f / std::max(ImGui::GetIO().Framerate, 1e-3f));
                ImGui::Text("Draws %u | pipeline changes %u | material changes %u",
                            stats.drawCalls, stats.pipelineChanges, stats.materialChanges);
                ImGui::Text("Subsets %u drawn / %u culled",
                            stats.subsetsVisible, stats.subsetsCulled);
                ImGui::Text("Triangles submitted %.2f M", stats.trianglesSubmitted / 1e6);
                ImGui::Text("Shadow %u casters (%u too small), %.2f M tris",
                            stats.shadowDraws, stats.shadowSkipped,
                            stats.shadowTriangles / 1e6);
                ImGui::Text("  %u cascades, %u indirect calls",
                            kCascadeCount, stats.shadowIndirectCalls);

                ImGui::Separator();
                ImGui::TextUnformatted("Drag mouse: look   WASD: move   Q/E: down/up");
                ImGui::TextUnformatted("Shift: fast   Ctrl: slow");
                ImGui::SliderFloat("Speed", &camera.speed, sceneExtent * 0.005f,
                                   sceneExtent * 1.5f, "%.3f", ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Field of view", &camera.fovY, 0.5f, 2.0f);
                if (ImGui::Button("Reset camera")) resetCamera();

                ImGui::Separator();
                ImGui::SliderFloat("Sun azimuth", &sunAzimuth, -3.14159f, 3.14159f);
                ImGui::SliderFloat("Sun elevation", &sunElevation, -0.2f, 1.5f);
                ImGui::SliderFloat("Sun intensity", &sunIntensity, 0.0f, 12.0f);
                ImGui::ColorEdit3("Sun colour", sunColor);
                ImGui::SliderFloat("Ambient", &ambientIntensity, 0.0f, 3.0f);
                ImGui::ColorEdit3("Sky", skyColor);
                ImGui::ColorEdit3("Ground bounce", groundColor);
                ImGui::SliderFloat("Exposure", &exposure, 0.05f, 6.0f, "%.2f",
                                   ImGuiSliderFlags_Logarithmic);

                ImGui::Separator();
                ImGui::Checkbox("Shadows", &shadowsEnabled);
                ImGui::SameLine();
                ImGui::Checkbox("Show cascades", &cascadeDebug);
                ImGui::SliderFloat("Shadow strength", &shadowStrength, 0.0f, 1.0f);
                ImGui::SliderFloat("Shadow distance", &shadowDistanceFraction, 0.05f, 1.0f);
                ImGui::SliderFloat("Normal bias (texels)", &shadowNormalBias, 0.0f, 6.0f);
                ImGui::SliderFloat("Constant bias (texels)", &shadowConstantBiasTexels, 0.0f, 4.0f);
                ImGui::SliderFloat("PCF radius (texels)", &shadowSoftness, 0.25f, 4.0f);

                ImGui::Separator();
                if (msaaSamples == SampleCount::One) {
                    ImGui::TextUnformatted("MSAA unsupported on this device");
                } else {
                    ImGui::Checkbox("MSAA", &useMsaa);
                    ImGui::SameLine();
                    ImGui::Text("(%ux)", static_cast<uint32_t>(msaaSamples));
                }
                if (maxAnisotropy > 1 &&
                    ImGui::SliderInt("Anisotropy", &anisotropy, 1,
                                     static_cast<int>(maxAnisotropy))) {
                    rebuildSampler(static_cast<uint32_t>(anisotropy));
                }
                ImGui::Checkbox("Frustum culling", &cullingEnabled);
                ImGui::SameLine();
                ImGui::Checkbox("Sorting", &sortingEnabled);
                ImGui::SliderFloat("Alpha cutoff", &alphaCutoff, 0.05f, 0.95f);

                ImGui::Separator();
                ImGui::SliderFloat("Near plane", &nearZ, sceneExtent * 0.00005f,
                                   sceneExtent * 0.02f, "%.4f", ImGuiSliderFlags_Logarithmic);
                ImGui::SliderFloat("Far plane", &farZ, sceneExtent * 0.5f, sceneExtent * 20.0f,
                                   "%.1f", ImGuiSliderFlags_Logarithmic);

                ImGui::Separator();
                const MemoryBudget budget = device->queryMemoryBudget();
                ImGui::Text("VRAM %.0f / %.0f MiB%s",
                            budget.deviceLocalUsedBytes / 1048576.0,
                            budget.deviceLocalBudgetBytes / 1048576.0,
                            budget.preciseBudget ? "" : " (estimated)");
                ImGui::Text("Geometry %.0f MiB | textures %.0f MiB",
                            mesh.geometryBytes() / 1048576.0,
                            textures.uploadedBytes() / 1048576.0);

                ImGui::End();
            }
            ImGui::Render();

            // ── Shadow passes, before anything reads their result ──
            std::shared_ptr<CommandBuffer> cmd = helper::createCommandBuffer(queue);
            renderShadows(cmd);

            // ── Scene pass ──
            const bool multisampled = useMsaa && msaaSamples != SampleCount::One;
            std::shared_ptr<RenderPassDescriptor> scenePass = helper::createRenderPassDescriptor();
            if (multisampled) {
                scenePass->setColorAttachment(0, msaaColorTarget, true, clearColor);
                // Resolved as part of ending the pass, so the averaging costs no extra draw.
                scenePass->setResolveAttachment(0, target);
                scenePass->setDepthStencilAttachment(msaaDepthTarget, true, 0.0f, false, 0);
            } else {
                scenePass->setColorAttachment(0, target, true, clearColor);
                scenePass->setDepthStencilAttachment(depthTarget, true, 0.0f, false, 0);
            }

            cmd->beginRenderPass(scenePass);
            recordScene(cmd, multisampled);
            cmd->endRenderPass();

            // ── Interface pass, over the finished frame ──
            cmd->beginRenderPass(uiPass);
            helper::renderInternalImgui(cmd);
            cmd->endRenderPass();

            cmd->present(target);
            cmd->commit();
        }

        helper::shutdownImgui();
        ImGui_ImplGlfw_Shutdown();
        ImGui::DestroyContext();
        glfwDestroyWindow(window);
        glfwTerminate();
    }
}
