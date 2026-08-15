#include "VulkanRenderPassDescriptor.hpp"

#include <algorithm>
#include <array>
#include <stdexcept>

namespace dmrender {

    struct VulkanRenderPassDescriptorNativeData
    {
        std::array<VulkanRenderPassDescriptor::Attachment, kMaxColorAttachments> colors;
        /// One past the highest index ever passed to setColorAttachment().
        uint32_t colorCount = 0;
        VulkanRenderPassDescriptor::Attachment depthStencil;
        bool clearStencil = false;
        VkRenderPass resolvedRenderPass = VK_NULL_HANDLE;
    };

    VulkanRenderPassDescriptor::VulkanRenderPassDescriptor()
        : m_data(std::make_unique<VulkanRenderPassDescriptorNativeData>())
    {
    }

    VulkanRenderPassDescriptor::~VulkanRenderPassDescriptor() = default;

    void VulkanRenderPassDescriptor::setColorAttachment(uint32_t index,
                                                        const std::shared_ptr<GImage>& image,
                                                        bool clear,
                                                        const ClearValue& clearValue)
    {
        if (index >= kMaxColorAttachments) {
            throw std::runtime_error("VulkanRenderPassDescriptor: colour attachment index is out of range");
        }
        m_data->colors[index].image = image;
        m_data->colors[index].clear = clear;
        m_data->colors[index].clearValue = clearValue;
        m_data->colorCount = std::max(m_data->colorCount, index + 1);
    }

    uint32_t VulkanRenderPassDescriptor::colorAttachmentCount() const
    {
        return m_data->colorCount;
    }

    void VulkanRenderPassDescriptor::setDepthStencilAttachment(const std::shared_ptr<GImage>& image,
                                                               bool clearDepth,
                                                               float depthValue,
                                                               bool clearStencil,
                                                               uint32_t stencilValue)
    {
        m_data->depthStencil.image = image;
        m_data->depthStencil.clear = clearDepth;
        m_data->depthStencil.clearValue.depth = depthValue;
        m_data->depthStencil.clearValue.stencil = stencilValue;
        m_data->clearStencil = clearStencil;
    }

    void* VulkanRenderPassDescriptor::nativeHandle()
    {
        return (void*)&m_data->resolvedRenderPass;
    }

    const VulkanRenderPassDescriptor::Attachment& VulkanRenderPassDescriptor::colorAttachment(uint32_t index) const
    {
        if (index >= kMaxColorAttachments) {
            throw std::runtime_error("VulkanRenderPassDescriptor: colour attachment index is out of range");
        }
        return m_data->colors[index];
    }

    const VulkanRenderPassDescriptor::Attachment& VulkanRenderPassDescriptor::depthStencilAttachment() const
    {
        return m_data->depthStencil;
    }

    bool VulkanRenderPassDescriptor::clearStencil() const { return m_data->clearStencil; }

    void VulkanRenderPassDescriptor::setResolvedRenderPass(VkRenderPass renderPass)
    {
        m_data->resolvedRenderPass = renderPass;
    }

} // namespace dmrender
