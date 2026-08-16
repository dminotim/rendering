//
// Created by Artem Avdoshkin on 12.07.2025.
//

#ifndef RENDERING_VULKANDEVICE_HPP
#define RENDERING_VULKANDEVICE_HPP

#include <vulkan/vulkan.h>

#include <set>

#include "Device.hpp"
#include "PipelineState.hpp"   // BufferSlotLayout
#include "Surface.hpp"
#include "VulkanMemoryAllocator.hpp"

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
        /// Set when this colour attachment is multisampled and resolves into a companion image.
        bool hasResolve = false;
        /// Resting layout of the resolve target, meaningful only when hasResolve is set.
        VkImageLayout resolveRestingLayout = VK_IMAGE_LAYOUT_UNDEFINED;

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
        /// Samples per pixel; every attachment in the pass shares this.
        VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;

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
            const ImageDesc& desc,
            const void* initialData
        ) override;

        std::shared_ptr<GSampler> createSampler(
            const SamplerDesc& desc,
            const std::string& debugName
        ) override;

        MemoryBudget queryMemoryBudget() const override;

        SampleCount maxSupportedSampleCount() const override;
        uint32_t maxSupportedAnisotropy() const override;

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
         * @struct PipelineLayoutSet
         * @brief The three cached layout objects a pipeline needs, for one buffer slot layout.
         */
        struct PipelineLayoutSet {
            VkDescriptorSetLayout bufferSet = VK_NULL_HANDLE;
            VkDescriptorSetLayout textureSet = VK_NULL_HANDLE;
            VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        };

        /**
         * @brief Returns cached layouts for @p slots, creating them on first use.
         *
         * A descriptor set layout fixes each binding's type, so a pipeline whose shader reads a
         * storage buffer at slot 2 needs a different layout from one that reads a uniform block
         * there. Rather than hardcoding one arrangement, the device keeps a small table keyed on
         * which slots are storage — an application typically ends up with two or three entries,
         * built once at startup.
         *
         * Caching also fixes an older wastefulness: every pipeline used to create, own and destroy
         * its own identical copies of these objects.
         *
         * @note The device owns the returned handles; do not destroy them.
         */
        const PipelineLayoutSet& acquirePipelineLayout(const BufferSlotLayout& slots);

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

        /// @brief Grows the shared staging buffer so it can hold at least @p size bytes.
        void ensureStagingCapacity(VkDeviceSize size);

        /**
         * @brief Uploads tightly packed pixels into one mip level of an image.
         *
         * Handles the layout transitions around the copy: the level is moved to
         * TRANSFER_DST_OPTIMAL, written, then moved to @p finalLayout. Synchronous, like the
         * buffer upload path.
         *
         * @param image The destination image.
         * @param width Width of this mip level in pixels.
         * @param height Height of this mip level in pixels.
         * @param mipLevel Which level to write.
         * @param data Tightly packed pixels.
         * @param size Size of @p data in bytes.
         * @param finalLayout The layout to leave the level in.
         */
        void uploadToImage(VkImage image,
                           uint32_t width,
                           uint32_t height,
                           uint32_t depth,
                           uint32_t mipLevel,
                           uint32_t arrayLayer,
                           const void* data,
                           VkDeviceSize size,
                           VkImageLayout finalLayout);

        /**
         * @brief Fills levels 1..@p mipLevels-1 by blitting each level down from the one above.
         *
         * Every level ends in @p finalLayout. Requires the format to advertise
         * VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT, which is checked and reported.
         */
        void generateMipmaps(VkImage image,
                             VkFormat format,
                             uint32_t width,
                             uint32_t height,
                             uint32_t depth,
                             uint32_t mipLevels,
                             uint32_t arrayLayers,
                             VkImageLayout finalLayout);

        /**
         * @brief Copies one mip level of an image back into CPU memory.
         *
         * The reverse of uploadToImage(): the level is moved to TRANSFER_SRC_OPTIMAL, copied
         * into the staging buffer, moved back to @p currentLayout, and then read out on the CPU.
         *
         * @param image The source image.
         * @param width Width of this mip level in pixels.
         * @param height Height of this mip level in pixels.
         * @param mipLevel Which level to read.
         * @param destination Buffer to fill with tightly packed pixels.
         * @param size Number of bytes to read.
         * @param currentLayout The layout the level is in, and is left in.
         */
        void readbackFromImage(VkImage image,
                               uint32_t width,
                               uint32_t height,
                               uint32_t depth,
                               uint32_t mipLevel,
                               uint32_t arrayLayer,
                               void* destination,
                               VkDeviceSize size,
                               VkImageLayout currentLayout);

        /// @brief Copies a range of a device-local buffer back into CPU memory.
        void readbackFromBuffer(VkBuffer source,
                                VkDeviceSize sourceOffset,
                                void* destination,
                                VkDeviceSize size);

        /// @brief Begins recording on the shared one-shot transfer command buffer.
        VkCommandBuffer beginTransferCommands();

        /// @brief Ends, submits and waits for the transfer command buffer.
        void endTransferCommands();

        /// @brief True when VK_EXT_memory_budget was available and enabled.
        bool hasMemoryBudgetExtension() const;

        /**
         * @brief The device's memory suballocator.
         *
         * Every buffer and image allocates through this rather than calling vkAllocateMemory
         * directly, because drivers cap the number of allocations a process may make.
         */
        VulkanMemoryAllocator& allocator() const;

        /// @brief True when the device supports BC block-compressed texture formats.
        bool supportsTextureCompressionBC() const;

        /// @brief True when one indirect draw call can issue more than one draw.
        bool supportsMultiDrawIndirect() const;

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
