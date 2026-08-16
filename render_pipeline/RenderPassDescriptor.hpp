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
         * Attachments must be configured from index 0 upwards with no gaps: a pass writing to
         * two targets sets index 0 and index 1. The pipeline used inside the pass must declare
         * exactly as many colour formats.
         *
         * @param index The attachment index, in [0, kMaxColorAttachments).
         * @param image The image resource to use as the render target.
         * @param clear A boolean flag indicating whether the attachment should be cleared
         *              at the beginning of the render pass.
         * @param clearValue The color value to use for clearing if 'clear' is true.
         * @param arrayLayer Which layer of an array or cubemap image to render into. This is how
         *                   a cubemap is filled a face at a time and how a shadow cascade
         *                   renders into its own slice. Must be 0 for non-array images.
         */
        virtual void setColorAttachment(uint32_t index,
                                        const std::shared_ptr<GImage>& image,
                                        bool clear,
                                        const ClearValue& clearValue,
                                        uint32_t arrayLayer = 0) = 0;

        /**
         * @brief The number of colour attachments configured so far.
         * @return One past the highest index passed to setColorAttachment(), or 0 if none.
         */
        virtual uint32_t colorAttachmentCount() const = 0;

        /**
         * @brief Names the single-sample image a multisample colour attachment resolves into.
         *
         * Multisample rendering writes several samples per pixel, which no shader can read. At
         * the end of the pass those samples are averaged into an ordinary image — the resolve
         * target — and it is that image later passes sample. Both backends do the resolve as
         * part of ending the pass, so it costs no extra draw.
         *
         * @param index The colour attachment index this resolve belongs to.
         * @param resolveImage A single-sample image with the same format and size as the
         *                     attachment at @p index.
         *
         * @note Only meaningful when the attachment at @p index is multisampled. Setting it on a
         *       single-sample attachment is an error.
         */
        virtual void setResolveAttachment(uint32_t index,
                                          const std::shared_ptr<GImage>& resolveImage) = 0;

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
                                               uint32_t stencilValue,
                                               uint32_t arrayLayer = 0) = 0;

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
