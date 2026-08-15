//
// Created by Artem Avdoshkin on 15.08.2025.
//

#ifndef RENDERING_PIPELINESTATE_HPP
#define RENDERING_PIPELINESTATE_HPP

#include <cstdint>

namespace dmrender {

    /**
     * @enum CompareOp
     * @brief How a new value is compared against the one already in a depth or stencil buffer.
     *
     * The fragment survives when the comparison passes. Values match Vulkan's VkCompareOp and
     * Metal's MTLCompareFunction one for one.
     */
    enum class CompareOp {
        Never,
        Less,
        Equal,
        LessOrEqual,
        Greater,
        NotEqual,
        GreaterOrEqual,
        Always
    };

    /**
     * @enum StencilOp
     * @brief What to do to a stencil value when a test resolves.
     */
    enum class StencilOp {
        Keep,
        Zero,
        Replace,
        IncrementClamp,
        DecrementClamp,
        Invert,
        IncrementWrap,
        DecrementWrap
    };

    /**
     * @enum BlendFactor
     * @brief The coefficient a colour is multiplied by before the blend operation.
     *
     * "Src" is the value the fragment shader produced; "Dst" is what is already in the
     * attachment.
     */
    enum class BlendFactor {
        Zero,
        One,
        SrcColor,
        OneMinusSrcColor,
        DstColor,
        OneMinusDstColor,
        SrcAlpha,
        OneMinusSrcAlpha,
        DstAlpha,
        OneMinusDstAlpha
    };

    /**
     * @enum BlendOp
     * @brief How the weighted source and destination colours are combined.
     */
    enum class BlendOp {
        Add,
        Subtract,
        ReverseSubtract,
        Min,
        Max
    };

    /**
     * @enum ColorComponent
     * @brief A bitmask selecting which channels a pipeline is allowed to write.
     */
    enum class ColorComponent : uint32_t {
        None  = 0,
        R     = 1 << 0,
        G     = 1 << 1,
        B     = 1 << 2,
        A     = 1 << 3,
        All   = R | G | B | A
    };

    inline ColorComponent operator|(ColorComponent a, ColorComponent b) {
        return static_cast<ColorComponent>(static_cast<uint32_t>(a) | static_cast<uint32_t>(b));
    }
    inline bool hasFlag(ColorComponent flags, ColorComponent flag) {
        return (static_cast<uint32_t>(flags) & static_cast<uint32_t>(flag)) == static_cast<uint32_t>(flag);
    }

    /**
     * @enum CullMode
     * @brief Which triangle facing is discarded before rasterisation.
     */
    enum class CullMode { None, Front, Back };

    /**
     * @enum FrontFace
     * @brief The winding order that counts as front-facing.
     */
    enum class FrontFace { CounterClockwise, Clockwise };

    /**
     * @enum PolygonMode
     * @brief Whether triangles are filled or drawn as their edges.
     * @note PolygonMode::Line needs the `fillModeNonSolid` feature on Vulkan. The backend
     *       reports an error rather than silently filling if the device lacks it.
     */
    enum class PolygonMode { Fill, Line };

    /**
     * @struct BlendState
     * @brief Per-colour-attachment blending configuration.
     *
     * The defaults describe opaque rendering: blending off, all channels written. Note that with
     * multiple render targets each attachment carries its own state, so a pass can composite one
     * target while overwriting another.
     */
    struct BlendState {
        bool enabled = false;

        BlendFactor srcColorFactor = BlendFactor::One;
        BlendFactor dstColorFactor = BlendFactor::Zero;
        BlendOp colorOp = BlendOp::Add;

        BlendFactor srcAlphaFactor = BlendFactor::One;
        BlendFactor dstAlphaFactor = BlendFactor::Zero;
        BlendOp alphaOp = BlendOp::Add;

        ColorComponent writeMask = ColorComponent::All;

        /// @brief Standard source-alpha transparency: src*a + dst*(1-a).
        static BlendState alphaBlend() {
            BlendState state{};
            state.enabled = true;
            state.srcColorFactor = BlendFactor::SrcAlpha;
            state.dstColorFactor = BlendFactor::OneMinusSrcAlpha;
            state.srcAlphaFactor = BlendFactor::One;
            state.dstAlphaFactor = BlendFactor::OneMinusSrcAlpha;
            return state;
        }

        /// @brief Additive blending, as used for glows and particles: src + dst.
        static BlendState additive() {
            BlendState state{};
            state.enabled = true;
            state.srcColorFactor = BlendFactor::One;
            state.dstColorFactor = BlendFactor::One;
            state.srcAlphaFactor = BlendFactor::One;
            state.dstAlphaFactor = BlendFactor::One;
            return state;
        }
    };

    /**
     * @struct StencilOpState
     * @brief The stencil configuration for one triangle facing.
     */
    struct StencilOpState {
        StencilOp failOp = StencilOp::Keep;       ///< Applied when the stencil test fails.
        StencilOp passOp = StencilOp::Keep;       ///< Applied when both stencil and depth pass.
        StencilOp depthFailOp = StencilOp::Keep;  ///< Applied when stencil passes but depth fails.
        CompareOp compareOp = CompareOp::Always;
        uint32_t compareMask = 0xFF;              ///< Bits of the stored value the test reads.
        uint32_t writeMask = 0xFF;                ///< Bits the operations may modify.
        uint32_t reference = 0;                   ///< Value the test compares against.
    };

    /**
     * @struct DepthStencilState
     * @brief Depth and stencil testing configuration.
     *
     * The defaults disable both, matching a pipeline that renders into colour only. Enabling
     * depth testing requires the render pass to actually have a depth attachment and the
     * pipeline's RenderTargetFormat to name its format.
     */
    struct DepthStencilState {
        bool depthTestEnabled = false;
        bool depthWriteEnabled = false;
        CompareOp depthCompareOp = CompareOp::Less;

        bool stencilTestEnabled = false;
        StencilOpState front{};
        StencilOpState back{};

        /// @brief Ordinary opaque geometry: test and write with a less-than comparison.
        static DepthStencilState depthTestAndWrite() {
            DepthStencilState state{};
            state.depthTestEnabled = true;
            state.depthWriteEnabled = true;
            return state;
        }

        /// @brief Transparent geometry: test against existing depth but leave it unchanged.
        static DepthStencilState depthTestOnly() {
            DepthStencilState state{};
            state.depthTestEnabled = true;
            state.depthWriteEnabled = false;
            state.depthCompareOp = CompareOp::LessOrEqual;
            return state;
        }
    };

    /**
     * @struct RasterizerState
     * @brief Triangle setup and rasterisation configuration.
     *
     * The defaults match what both backends do when told nothing: fill triangles, cull nothing.
     * Culling nothing is the safe default because it makes winding order irrelevant, which is
     * one fewer thing to differ between the two APIs.
     */
    struct RasterizerState {
        CullMode cullMode = CullMode::None;
        FrontFace frontFace = FrontFace::CounterClockwise;
        PolygonMode polygonMode = PolygonMode::Fill;

        /**
         * @brief Offsets generated depth values, in units of the depth buffer's precision.
         *
         * Chiefly used to stop shadow-map geometry from self-shadowing. Both terms must be zero
         * to disable the bias entirely.
         */
        float depthBiasConstant = 0.0f;
        float depthBiasSlope = 0.0f;

        bool depthBiasEnabled() const {
            return depthBiasConstant != 0.0f || depthBiasSlope != 0.0f;
        }
    };

} // namespace dmrender
#endif //RENDERING_PIPELINESTATE_HPP
