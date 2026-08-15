//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_METALPIPELINE_HPP
#define RENDERING_METALPIPELINE_HPP
#include <memory>
#include "Device.hpp"
#include "ShaderFunction.hpp"
#include "Pipeline.hpp"
namespace dmrender {
    struct MetalPipelineNativeData;

    /**
     * @class MetalPipeline
     * @brief Metal's answer to a pipeline state object, which is really three objects.
     *
     * Vulkan bakes shaders, blending, depth, stencil and rasterisation into a single immutable
     * VkPipeline. Metal splits the same information up:
     *
     *  - shaders, attachment formats and blending live in an MTLRenderPipelineState,
     *  - depth and stencil live in a separate MTLDepthStencilState,
     *  - cull mode, winding, fill mode, depth bias and the stencil reference value are commands
     *    issued on the render command encoder.
     *
     * This class owns the first two and remembers the third group, so that
     * `MetalCommandBuffer::setRenderPipeline()` can apply all of it at once and binding a
     * Pipeline means the same thing on both backends.
     */
    class MetalPipeline : public Pipeline {

    public:
        MetalPipeline(const std::shared_ptr<Device>& device, const PipelineDesc& desc);

        ~MetalPipeline() override;

        void *nativeHandle() const override;
        const std::string& debugName() const override;

        /// @return The id<MTLDepthStencilState> to bind, or nullptr when neither test is enabled.
        void* depthStencilState() const;

        /// @brief Applies the encoder-side state this pipeline carries.
        /// @param encoder An id<MTLRenderCommandEncoder>.
        void applyEncoderState(void* encoder) const;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalPipelineNativeData> m_data;
        std::string m_debugName;
    };
}
#endif //RENDERING_METALPIPELINE_HPP
