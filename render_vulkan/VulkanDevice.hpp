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
     * @struct RenderPassAttachmentKey
     * @brief One attachment's contribution to a render pass's identity.
     */
    struct RenderPassAttachmentKey {
        VkFormat format = VK_FORMAT_UNDEFINED;
        /// True when the pass clears this attachment; false means its contents are loaded.
        bool clear = true;
        /// The layout the attachment rests in outside the pass. See VulkanImage::restingLayout().
        VkImageLayout restingLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        bool operator<(const RenderPassAttachmentKey& other) const;
    };

    /**
     * @struct RenderPassKey
     * @brief Everything a VkRenderPass object depends on in this backend.
     *
     * Metal builds a fresh MTLRenderPassDescriptor every frame; Vulkan needs a VkRenderPass
     * object instead, and creating one per frame would be wasteful. The device therefore
     * caches render passes by the small set of properties that actually change.
     *
     * With multiple render targets the identity grows a colour attachment per output, each with
     * its own format, load op and resting layout — a pass writing a swapchain image and an
     * offscreen texture at once has two different resting layouts in one key.
     *
     * @note Two render passes are "compatible" (in the Vulkan sense used by pipelines and by
     *       the ImGui backend) when their attachment counts, formats and sample counts match;
     *       load ops and layouts are irrelevant. That is why a pipeline can be built against the
     *       cached clear-variant and still be used inside the load-variant.
     */
    struct RenderPassKey {
        /// One entry per colour attachment, in fragment shader output order.
        std::vector<RenderPassAttachmentKey> colors;
        /// Format VK_FORMAT_UNDEFINED means the pass has no depth attachment.
        RenderPassAttachmentKey depth;

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

        std::shared_ptr<GImage> createImage(
            ImageType type,
            ImageFormat format,
            uint32_t width,
            uint32_t height,
            ImageUsage usage,
            const std::string& debugName
        ) override;

        std::shared_ptr<GSampler> createSampler(
            const SamplerDesc& desc,
            const std::string& debugName
        ) override;

        MemoryBudget queryMemoryBudget() const override;

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
         * @brief Returns a cached VkFramebuffer for @p attachments, creating it on first use.
         *
         * The cache lives on the device rather than on the swapchain because a framebuffer may
         * now be built entirely out of offscreen images that no swapchain knows about. Whoever
         * destroys an image view is responsible for calling invalidateFramebuffersUsing() first.
         *
         * @param renderPass A render pass the framebuffer must be compatible with.
         * @param attachments The image views, colour attachments first then depth.
         * @param width Framebuffer width in pixels.
         * @param height Framebuffer height in pixels.
         */
        VkFramebuffer acquireFramebuffer(VkRenderPass renderPass,
                                         const std::vector<VkImageView>& attachments,
                                         uint32_t width,
                                         uint32_t height);

        /**
         * @brief Destroys every cached framebuffer that references @p view.
         * @note Must be called before the view itself is destroyed, and after the device is idle.
         */
        void invalidateFramebuffersUsing(VkImageView view);

        /**
         * @brief Picks a memory type index, preferring @p preferred and falling back to @p required.
         *
         * Used to express "device-local if the driver offers it, host-visible otherwise" in one
         * call, so an integrated GPU with no separate VRAM heap still gets a valid allocation.
         *
         * @param typeBits The memoryTypeBits from the resource's VkMemoryRequirements.
         * @param preferred Property flags to try first.
         * @param required Property flags to accept if @p preferred is unavailable.
         * @param[out] outFlags The property flags of the type that was actually chosen.
         * @return The chosen memory type index.
         */
        uint32_t selectMemoryType(uint32_t typeBits,
                                  VkMemoryPropertyFlags preferred,
                                  VkMemoryPropertyFlags required,
                                  VkMemoryPropertyFlags& outFlags) const;

        /**
         * @brief Copies CPU data into a device-local buffer through a staging buffer.
         *
         * Device-local memory on a discrete GPU is not CPU-addressable, so the bytes are written
         * into a host-visible staging buffer the device keeps around, then copied on the GPU with
         * vkCmdCopyBuffer. The submit is waited on before returning, which makes this a
         * synchronous, stalling operation — fine at resource creation time, deliberately
         * unattractive for per-frame updates (that is what BufferUsage::Dynamic is for).
         *
         * @param destination The device-local buffer to write into.
         * @param destinationOffset Byte offset within @p destination.
         * @param data The bytes to upload.
         * @param size How many bytes to upload.
         */
        void uploadToDeviceLocalBuffer(VkBuffer destination,
                                       VkDeviceSize destinationOffset,
                                       const void* data,
                                       VkDeviceSize size);

        /// @brief True when VK_EXT_memory_budget was available and enabled.
        bool hasMemoryBudgetExtension() const;

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
