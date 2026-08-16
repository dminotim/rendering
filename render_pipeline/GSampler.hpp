//
// Created by Artem Avdoshkin on 15.08.2025.
//

#ifndef RENDERING_GSAMPLER_HPP
#define RENDERING_GSAMPLER_HPP

#include <cstdint>
#include <string>

namespace dmrender {

    /**
     * @enum SamplerFilter
     * @brief How texels are selected when a sampled coordinate falls between them.
     */
    enum class SamplerFilter {
        Nearest, ///< Take the closest texel. Keeps a render target pixel-exact when rescaled.
        Linear   ///< Blend the neighbouring texels.
    };

    /**
     * @enum SamplerAddressMode
     * @brief What happens to texture coordinates outside [0, 1].
     */
    enum class SamplerAddressMode {
        Repeat,
        MirroredRepeat,
        ClampToEdge
    };

    /**
     * @struct SamplerDesc
     * @brief The complete description of a sampler.
     *
     * The defaults describe the sampler a full-screen composite pass wants: linear filtering,
     * clamped addressing, no mip selection.
     */
    struct SamplerDesc {
        SamplerFilter minFilter = SamplerFilter::Linear;
        SamplerFilter magFilter = SamplerFilter::Linear;

        /**
         * @brief How neighbouring mip levels are combined.
         *
         * Linear gives trilinear filtering; Nearest snaps to the closest level and can show
         * visible seams where the level changes.
         */
        SamplerFilter mipFilter = SamplerFilter::Linear;

        SamplerAddressMode addressU = SamplerAddressMode::ClampToEdge;
        SamplerAddressMode addressV = SamplerAddressMode::ClampToEdge;
        SamplerAddressMode addressW = SamplerAddressMode::ClampToEdge;

        /**
         * @brief Range of mip levels this sampler may read.
         *
         * The default spans every level an image has. Raising minLod forces a blurrier level,
         * which is occasionally useful for cheap depth-of-field or for hiding aliasing.
         */
        float minLod = 0.0f;
        float maxLod = 1000.0f;

        /**
         * @brief How many samples the hardware may take along the axis of anisotropy. 1 is off.
         *
         * A pixel viewed at a grazing angle — a floor receding towards the horizon is the standard
         * case — covers an elongated footprint in the texture: a few texels across, dozens along.
         * Ordinary trilinear filtering can only pick a square region, so it selects the mip level
         * that covers the *long* axis and blurs the short one away with it. The floor turns to mush
         * well before the horizon.
         *
         * Anisotropic filtering takes several samples along the long axis instead, keeping the
         * short axis sharp. It costs texture bandwidth roughly in proportion to the value, and on
         * every desktop GPU it is one of the cheapest quality wins available.
         *
         * Values above the device limit are clamped rather than rejected, so passing 16 is safe
         * everywhere. Ask Device::maxSupportedAnisotropy() when the number matters — for a quality
         * setting in a menu, say.
         *
         * @note Only affects minification. Magnified textures are unaffected, so a value above 1
         *       costs nothing on textures that are only ever seen close up.
         */
        uint32_t maxAnisotropy = 1;
    };

    /**
     * @class GSampler
     * @brief An abstract interface for the state that controls how a shader reads a texture.
     *
     * Both backends keep sampling state in an object separate from the texture itself
     * (id<MTLSamplerState>, VkSampler), so one sampler can be shared by every texture that
     * wants the same filtering. Samplers are immutable once created.
     */
    class GSampler {
    public:
        virtual ~GSampler() = default;

        // Prohibit copy and move operations. Samplers are unique resources.
        GSampler(const GSampler&) = delete;
        GSampler& operator=(const GSampler&) = delete;
        GSampler(GSampler&&) = delete;
        GSampler& operator=(GSampler&&) = delete;

        /**
         * @brief Retrieves the native, backend-specific handle for the sampler.
         * @return A void pointer to the native object (e.g., id<MTLSamplerState>, VkSampler*).
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Gets the description this sampler was created from.
         * @return A const reference to the SamplerDesc.
         */
        virtual const SamplerDesc& desc() const = 0;

        virtual const std::string& debugName() const = 0;

    protected:
        GSampler() = default;
    };

} // namespace dmrender
#endif //RENDERING_GSAMPLER_HPP
