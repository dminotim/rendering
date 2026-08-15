//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_DEVICE_HPP
#define RENDERING_DEVICE_HPP

#include <string>
#include <memory>
#include <vector>
#include "GBuffer.hpp"
#include "GImage.hpp"
#include "GSampler.hpp"
#include "Memory.hpp"

namespace dmrender
{
    /**
     * @enum DeviceExtension
     * @brief Defines features or extensions that a physical device can support.
     *
     * This abstracts backend-specific extensions (like Vulkan layers/extensions or Metal features)
     * into a unified list.
     */
    enum class DeviceExtension
    {
        Surface,    // Support for rendering to a window surface.
        SwapChain,  // Support for swapchain creation (often implied by Surface).
        Validation, // validation layer (awailable in vulkan only)
        // Add other extensions as needed, e.g., RayTracing, MeshShaders, etc.
    };

    /**
     * @struct DeviceId
     * @brief Contains identification information for a physical device.
     */
    struct DeviceId
    {
        std::string name;
        uint32_t id;
        // Could also include vendorID, deviceType (discrete, integrated), etc. in the future.
    };

    /**
     * @class Device
     * @brief An abstract interface for a logical graphics device.
     *
     * The Device is the primary interface for creating resources (buffers, textures, pipelines)
     * and interacting with the GPU. It represents a logical connection to a physical device.
     */
    class Device {
    public:
        virtual ~Device() = default;

        // Prohibit copy and move operations. A device should be a unique resource.
        Device(const Device&) = delete;
        Device& operator=(const Device&) = delete;
        Device(Device&&) = delete;
        Device& operator=(Device&&) = delete;

        /**
         * @brief Checks if a specific extension is available on this device.
         * @param extension The extension to check for.
         * @return True if the extension is supported, false otherwise.
         */
        virtual bool isExtensionAvailable(DeviceExtension extension) const = 0;

        /**
         * @brief Activates a specific extension for use.
         * @param extension The extension to activate.
         * @return True if activation was successful, false otherwise.
         * @note Must be called before creating resources that depend on the extension.
         */
        virtual bool activateExtension(DeviceExtension extension) = 0;

        /**
         * @brief Creates a GPU buffer resource.
         * @param type The type of the buffer (Vertex, Index, etc.).
         * @param usage A hint about how the buffer will be used (Static, Dynamic).
         * @param size The total size of the buffer in bytes.
         * @param initialData Optional pointer to data to initialize the buffer with.
         * @param debugName An optional name for debugging purposes.
         * @return A shared pointer to the created GBuffer, or nullptr on failure.
         */
        virtual std::shared_ptr<GBuffer> createBuffer(
                BufferType type,
                BufferUsage usage,
                size_t size,
                const void* initialData = nullptr,
                const std::string& debugName = ""
        ) = 0;

        /**
         * @brief Creates a GPU image resource.
         *
         * Covers both roles an image plays here. Pass
         * `ImageUsage::ColorTarget | ImageUsage::Sampled` for an offscreen render target a later
         * pass samples, or `ImageUsage::Sampled` plus @p initialData for a texture uploaded from
         * the CPU.
         *
         * @param desc The image's dimensions, format, mip count and usage. The usage must be
         *             complete: both backends bake it into the native object at creation time
         *             and neither can widen it afterwards.
         * @param initialData Optional tightly packed pixels for mip level 0. When supplied on an
         *                    image with more than one level, the rest of the chain is generated
         *                    automatically.
         * @return A shared pointer to the created GImage, or nullptr on failure.
         */
        virtual std::shared_ptr<GImage> createImage(
                const ImageDesc& desc,
                const void* initialData = nullptr
        ) = 0;

        /**
         * @brief Creates a sampler describing how shaders read textures.
         * @param desc The sampling state.
         * @param debugName An optional name for debugging purposes.
         * @return A shared pointer to the created GSampler, or nullptr on failure.
         * @note Samplers are immutable and cheap to share; create a few and reuse them rather
         *       than making one per texture.
         */
        virtual std::shared_ptr<GSampler> createSampler(
                const SamplerDesc& desc,
                const std::string& debugName = ""
        ) = 0;

        /**
         * @brief Takes a snapshot of device-local memory availability.
         *
         * Resource creation already consults this internally — a static buffer that would not
         * fit in the remaining budget is placed in host-visible memory instead of failing — so
         * calling it is only necessary when the application wants to size its own allocations,
         * report usage, or degrade quality before the budget runs out.
         *
         * @return The current MemoryBudget. Cheap enough to call once per frame.
         */
        virtual MemoryBudget queryMemoryBudget() const = 0;

        /**
         * @brief The highest sample count this device supports for colour and depth targets.
         *
         * Four is universally available on desktop hardware; eight and sixteen are common but
         * not guaranteed. Query rather than assume, and clamp your preferred setting to this.
         */
        virtual SampleCount maxSupportedSampleCount() const = 0;

        /**
         * @brief Retrieves the native, backend-specific handle for the logical device.
         * @return A void pointer to the native object (e.g., id<MTLDevice>, VkDevice).
         * @note Use with caution, as this breaks the abstraction layer.
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Gets the identifier for the physical device this logical device was created from.
         * @return A const reference to the DeviceId struct.
         */
        const DeviceId& getDeviceId() const {
            return m_deviceId;
        }

    protected:
        /**
         * @brief Protected constructor to be called by derived classes.
         * @param id The identifier of the physical device.
         */
        explicit Device(DeviceId id) : m_deviceId(std::move(id)) {}

        /// @brief The identifier of the physical device associated with this logical device.
        DeviceId m_deviceId;
    };

} // namespace dmrender
#endif //RENDERING_DEVICE_HPP
