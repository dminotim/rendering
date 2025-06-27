//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_METALRENDERPASSDESCRIPTOR_HPP
#define RENDERING_METALRENDERPASSDESCRIPTOR_HPP
#include "RenderPassDescriptor.hpp"
#include "GImage.hpp"

namespace dmrender {

    struct MetalRenderPassDescriptorNativeData;

    class MetalRenderPassDescriptor : public RenderPassDescriptor {
    public:
        MetalRenderPassDescriptor();

        ~MetalRenderPassDescriptor() override;

        void setColorAttachment(uint32_t index,
                                const std::shared_ptr<GImage>& image,
                                bool clear,
                                const ClearValue &clearValue) override;

        void setDepthStencilAttachment(const std::shared_ptr<GImage>& image,
                                       bool clearDepth,
                                       float depthValue,
                                       bool clearStencil,
                                       uint32_t stencilValue) override;

        void *nativeHandle() override;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalRenderPassDescriptorNativeData> m_data;
    };
}
#endif //RENDERING_METALRENDERPASSDESCRIPTOR_HPP
