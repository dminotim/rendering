//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_RENDERPASSDESCRIPTOR_HPP
#define RENDERING_RENDERPASSDESCRIPTOR_HPP
#include <memory>
#include "GImage.hpp"

namespace dmrender {

    /**
     * @class RenderPassDescriptor
     * @brief An interface for describing the attachments of a render pass.
     *
     * This object specifies which images (textures) will be used as render targets
     * (color, depth, stencil) and how they should be treated at the beginning
     * of the render pass (e.g., whether to clear them before rendering).
     * An instance of this class is used to begin a render pass on a CommandBuffer.
     */
    class RenderPassDescriptor {
    public:
        virtual ~RenderPassDescriptor() = default;

        /**
         * @brief Configures a color attachment at a specific index.
         *
         * Each color attachment corresponds to an output location in a fragment shader
         * (e.g., `[[color(0)]]` in Metal Shading Language).
         *
         * @param index The attachment index (e.g., 0 for the first color target).
         * @param image The image resource to use as the render target.
         * @param clear A boolean flag indicating whether the attachment should be cleared
         *              at the beginning of the render pass.
         * @param clearValue The color value to use for clearing if 'clear' is true.
         */
        virtual void setColorAttachment(uint32_t index,
                                        const std::shared_ptr<GImage>& image,
                                        bool clear,
                                        const ClearValue& clearValue) = 0;

        /**
         * @brief Configures the depth and stencil attachment for the render pass.
         *
         * This sets up the image resource that will be used for depth and/or stencil testing.
         *
         * @param image The image resource to use as the depth/stencil buffer.
         * @param clearDepth A flag indicating if the depth buffer should be cleared.
         * @param depthValue The value to clear the depth buffer with.
         * @param clearStencil A flag indicating if the stencil buffer should be cleared.
         * @param stencilValue The value to clear the stencil buffer with.
         */
        virtual void setDepthStencilAttachment(const std::shared_ptr<GImage>& image,
                                               bool clearDepth,
                                               float depthValue,
                                               bool clearStencil,
                                               uint32_t stencilValue) = 0;

        /**
         * @brief Retrieves the native, backend-specific handle for the descriptor.
         *
         * This handle can be used for direct interaction with the underlying graphics API
         * (e.g., to pass an `MTLRenderPassDescriptor*` to a library like ImGui).
         *
         * @return A void pointer to the native object.
         */
        virtual void* nativeHandle() = 0;
    };

} // namespace dmrender
#endif //RENDERING_RENDERPASSDESCRIPTOR_HPP
