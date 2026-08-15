//
// Created by Artem Avdoshkin on 14.08.2025.
//

#ifndef RENDERING_VULKANRENDERPASSDESCRIPTOR_HPP
#define RENDERING_VULKANRENDERPASSDESCRIPTOR_HPP

#include <vulkan/vulkan.h>

#include <memory>

#include "RenderPassDescriptor.hpp"

namespace dmrender {

    struct VulkanRenderPassDescriptorNativeData;

    /**
     * @class VulkanRenderPassDescriptor
     * @brief Pure description of a render pass: attachments, clear flags, clear values.
     *
     * On Metal this maps one-to-one onto MTLRenderPassDescriptor. Vulkan splits the same
     * information across a VkRenderPass (formats and load/store ops), a VkFramebuffer (the actual
     * image views) and VkRenderPassBeginInfo (clear values), and none of those can be built
     * without a device — which the parameterless `helper::createRenderPassDescriptor()` does not
     * have. So this object stays a plain value holder and
     * `VulkanCommandBuffer::beginRenderPass()` resolves it against the device it already owns.
     */
    class VulkanRenderPassDescriptor : public RenderPassDescriptor {
    public:
        /// @brief One configured attachment slot.
        struct Attachment {
            std::shared_ptr<GImage> image;
            bool clear = false;
            ClearValue clearValue{};

            bool isValid() const { return image != nullptr; }
        };

        VulkanRenderPassDescriptor();
        ~VulkanRenderPassDescriptor() override;

        void setColorAttachment(uint32_t index,
                                const std::shared_ptr<GImage>& image,
                                bool clear,
                                const ClearValue& clearValue) override;

        uint32_t colorAttachmentCount() const override;

        void setResolveAttachment(uint32_t index, const std::shared_ptr<GImage>& resolveImage) override;

        /// @brief The resolve target for colour attachment @p index, or nullptr if none is set.
        const std::shared_ptr<GImage>& resolveAttachment(uint32_t index) const;

        void setDepthStencilAttachment(const std::shared_ptr<GImage>& image,
                                       bool clearDepth,
                                       float depthValue,
                                       bool clearStencil,
                                       uint32_t stencilValue) override;

        /**
         * @brief Pointer to the VkRenderPass this descriptor resolved to.
         * @return Points to VK_NULL_HANDLE until the descriptor has been used in a
         *         `beginRenderPass()` call, because only then is a device available.
         */
        void* nativeHandle() override;

        /// @brief The attachment configured at @p index; invalid (image == nullptr) if never set.
        const Attachment& colorAttachment(uint32_t index) const;

        const Attachment& depthStencilAttachment() const;
        bool clearStencil() const;

        /// @brief Records which VkRenderPass a command buffer resolved this descriptor to.
        void setResolvedRenderPass(VkRenderPass renderPass);

    private:
        std::unique_ptr<VulkanRenderPassDescriptorNativeData> m_data;
    };

} // namespace dmrender

#endif //RENDERING_VULKANRENDERPASSDESCRIPTOR_HPP
