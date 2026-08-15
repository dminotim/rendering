//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_PIPELINE_HPP
#define RENDERING_PIPELINE_HPP

#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

#include "GImage.hpp" // For ImageFormat
#include "PipelineState.hpp"

namespace dmrender {

    class ShaderFunction;

    /**
     * @struct RenderTargetFormat
     * @brief Describes the pixel formats of render targets that a pipeline will write to.
     *
     * A pipeline is only valid inside a render pass whose attachments match this description,
     * so the number of colour formats here must equal the number of colour attachments the
     * render pass declares, and equal the number of outputs the fragment shader writes.
     * A shader writing two outputs cannot be used with a single-attachment pipeline.
     */
    struct RenderTargetFormat {
        /**
         * @brief The pixel format of each colour attachment, in fragment shader output order.
         *
         * Entry @c i corresponds to `[[color(i)]]` in Metal and `layout(location = i) out` in
         * GLSL. Must hold between 1 and @c kMaxColorAttachments entries.
         */
        std::vector<ImageFormat> colorFormats;

        /**
         * @brief The pixel format of the depth attachment.
         */
        ImageFormat depthFormat = ImageFormat::Undefined;

        /**
         * @brief The pixel format of the stencil attachment.
         * @note Often the same as the depth attachment if a combined depth/stencil format is used.
         */
        ImageFormat stencilFormat = ImageFormat::Undefined;

        /**
         * @brief Samples per pixel, which must match the render pass this pipeline runs in.
         *
         * A pipeline built for four samples cannot be used in a single-sample pass or the other
         * way round, so switching MSAA on and off at runtime means keeping both pipelines.
         */
        SampleCount sampleCount = SampleCount::One;

        /**
         * @brief Convenience builder for the common single render target case.
         * @param color The colour attachment format.
         * @param depth An optional depth attachment format.
         */
        static RenderTargetFormat singleTarget(ImageFormat color,
                                               ImageFormat depth = ImageFormat::Undefined,
                                               SampleCount samples = SampleCount::One)
        {
            return RenderTargetFormat{ {color}, depth, ImageFormat::Undefined, samples };
        }

        /**
         * @brief Convenience builder for multiple render targets.
         * @param colors The colour attachment formats, in output order.
         * @param depth An optional depth attachment format.
         */
        static RenderTargetFormat multiTarget(std::initializer_list<ImageFormat> colors,
                                              ImageFormat depth = ImageFormat::Undefined,
                                              SampleCount samples = SampleCount::One)
        {
            return RenderTargetFormat{ std::vector<ImageFormat>(colors), depth,
                                       ImageFormat::Undefined, samples };
        }
    };

    /**
     * @struct PipelineDesc
     * @brief Everything that goes into a graphics pipeline state object.
     *
     * Fixed-function state is described here rather than hardcoded, so a pipeline can render
     * opaque geometry, composite a transparent overlay, or write a shadow map with a depth bias,
     * all through the same call.
     *
     * @note Which backend object each field ends up in differs a great deal. Vulkan bakes the
     *       lot into one immutable VkPipeline. Metal splits it: blending and formats belong to
     *       the MTLRenderPipelineState, depth and stencil become a separate MTLDepthStencilState,
     *       and culling, winding, fill mode and depth bias are commands on the encoder. The
     *       backend reassembles them so that binding one Pipeline applies all of it either way.
     */
    struct PipelineDesc {
        std::shared_ptr<ShaderFunction> vertexFunction;
        std::shared_ptr<ShaderFunction> fragmentFunction;

        /// @brief Formats of the attachments this pipeline writes.
        RenderTargetFormat targetFormat;

        /**
         * @brief Blending, one entry per colour attachment.
         *
         * When empty, every attachment gets the default opaque BlendState. When non-empty it
         * must have exactly as many entries as targetFormat.colorFormats.
         */
        std::vector<BlendState> blendStates;

        DepthStencilState depthStencil{};
        RasterizerState rasterizer{};

        std::string debugName;
    };

    /**
     * @class Pipeline
     * @brief An abstract interface for a graphics pipeline state object (PSO).
     *
     * A Pipeline encapsulates a large amount of GPU state, including shaders,
     * blending, rasterization, and depth/stencil settings, into a single immutable object.
     * They are typically expensive to create but cheap to bind for drawing.
     */
    class Pipeline {
    public:
        virtual ~Pipeline() = default;

        // Prohibit copy and move operations. Pipeline objects are unique resources.
        Pipeline(const Pipeline&) = delete;
        Pipeline& operator=(const Pipeline&) = delete;
        Pipeline(Pipeline&&) = delete;
        Pipeline& operator=(Pipeline&&) = delete;

        /**
         * @brief Retrieves the native, backend-specific handle for the pipeline state.
         * @return A void pointer to the native object (e.g., id<MTLRenderPipelineState>, VkPipeline).
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Gets the debug name assigned to this pipeline.
         * @return A string view of the debug name. An empty view means no name is set.
         */
        virtual const std::string& debugName() const = 0;

    protected:
        Pipeline() = default;
    };

} // namespace dmrender
#endif //RENDERING_PIPELINE_HPP
