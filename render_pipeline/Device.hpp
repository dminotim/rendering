//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_DEVICE_HPP
#define RENDERING_DEVICE_HPP

#include <string>
#include <memory>
#include <vector>
#include "GBuffer.hpp"

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
