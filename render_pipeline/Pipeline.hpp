//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_PIPELINE_HPP
#define RENDERING_PIPELINE_HPP

#include "GImage.hpp" // For ImageFormat

namespace dmrender {

    /**
     * @struct RenderTargetFormat
     * @brief Describes the pixel formats of render targets that a pipeline will write to.
     */
    struct RenderTargetFormat {
        /**
          * @brief The pixel format of the primary color attachment.
          * @note For multiple render targets (MRT), this could be expanded to a std::vector.
          */
        ImageFormat colorFormat = ImageFormat::Undefined;

        /**
         * @brief The pixel format of the depth attachment.
         */
        ImageFormat depthFormat = ImageFormat::Undefined;

        /**
         * @brief The pixel format of the stencil attachment.
         * @note Often the same as the depth attachment if a combined depth/stencil format is used.
         */
        ImageFormat stencilFormat = ImageFormat::Undefined;
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
