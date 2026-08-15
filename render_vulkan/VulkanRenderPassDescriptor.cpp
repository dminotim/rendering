#include "VulkanRenderPassDescriptor.hpp"

#include <stdexcept>

namespace dmrender {

    struct VulkanRenderPassDescriptorNativeData
    {
        VulkanRenderPassDescriptor::Attachment color;
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
        if (index != 0) {
            // Multiple render targets would need one VkAttachmentDescription per index and a
            // wider RenderPassKey; the interface allows it, this backend does not yet.
            throw std::runtime_error("VulkanRenderPassDescriptor: only colour attachment 0 is supported");
        }
        m_data->color.image = image;
        m_data->color.clear = clear;
        m_data->color.clearValue = clearValue;
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

    const VulkanRenderPassDescriptor::Attachment& VulkanRenderPassDescriptor::colorAttachment() const
    {
        return m_data->color;
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
