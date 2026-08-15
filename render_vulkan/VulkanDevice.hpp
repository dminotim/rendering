//
// Created by Artem Avdoshkin on 12.07.2025.
//

#ifndef RENDERING_VULKANDEVICE_HPP
#define RENDERING_VULKANDEVICE_HPP

#include <vulkan/vulkan.h>

#include <set>

#include "Device.hpp"
#include "Surface.hpp"

namespace dmrender {

    struct VulkanDeviceNativeData;

    /**
     * @struct RenderPassKey
     * @brief Everything a VkRenderPass object depends on in this backend.
     *
     * Metal builds a fresh MTLRenderPassDescriptor every frame; Vulkan needs a VkRenderPass
     * object instead, and creating one per frame would be wasteful. The device therefore
     * caches render passes by the small set of properties that actually change.
     *
     * @note Two render passes are "compatible" (in the Vulkan sense used by pipelines and by
     *       the ImGui backend) when their attachment formats and sample counts match, load and
     *       store ops are irrelevant. That is why a pipeline can be built against the cached
     *       clear-variant and still be used inside the load-variant.
     */
    struct RenderPassKey {
        VkFormat colorFormat = VK_FORMAT_UNDEFINED;
        VkFormat depthFormat = VK_FORMAT_UNDEFINED;
        bool clearColor = true;
        bool clearDepth = true;

        bool operator<(const RenderPassKey& other) const;
    };

    class VulkanDevice : public Device
    {
    public:

        static std::shared_ptr<Device>  createDefaultDevice(const std::shared_ptr<Surface>& surface);
        static std::shared_ptr<Device>  createDeviceById(const std::shared_ptr<Surface>& surface, const DeviceId& id);
        static std::vector<DeviceId> enumerateAvailableDevices();

        ~VulkanDevice() override;

        bool activateExtension(DeviceExtension ext) override;
        bool isExtensionAvailable(DeviceExtension ext) const override;

        std::shared_ptr<GBuffer> createBuffer(
            BufferType type,
            BufferUsage usage,
            size_t size,
            const void* initialData,
            const std::string& debugName
        ) override;

        void* nativeHandle() const override;
        void* getLogicalDevice() const;
        uint32_t getGraphicsFamilyIndex() const;
        uint32_t getPresentFamilyIndex() const;

        // --- Typed accessors, preferred inside the backend over the void* ones above ---

        VkPhysicalDevice physicalDevice() const;
        VkDevice logicalDevice() const;
        const VkPhysicalDeviceProperties& properties() const;

        /**
         * @brief Returns a cached VkRenderPass matching @p key, creating it on first use.
         * @note The device owns the returned handle; do not destroy it.
         */
        VkRenderPass acquireRenderPass(const RenderPassKey& key);

        /**
         * @brief Index of the frame slot currently being recorded, in [0, kFramesInFlight).
         *
         * Set by VulkanCommandQueues at the start of every frame. BufferUsage::Dynamic buffers
         * use it to pick which of their internal regions to write, so that updating a uniform
         * buffer never touches memory a still-executing frame is reading.
         */
        uint32_t currentFrameSlot() const;
        void setCurrentFrameSlot(uint32_t slot);

        VulkanDevice(const std::shared_ptr<Surface>& surface, DeviceId id, void* nativeDevice);
    private:
        /// @brief Pointer to implementation (PIMPL) to hide Vulkan-specific details.
        std::unique_ptr<VulkanDeviceNativeData> m_data;
        std::set<DeviceExtension> m_activatedInstanceExtensions;
    };

}


#endif //RENDERING_VULKANDEVICE_HPP
