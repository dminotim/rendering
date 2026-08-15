//
// Created by Artem Avdoshkin on 13.06.2025.
//

#ifndef RENDERING_METALUTILSCPP_HPP
#define RENDERING_METALUTILSCPP_HPP
#import <Metal/Metal.h>
#include "GImage.hpp"
#include "GSampler.hpp"
#import "Device.hpp"

namespace dmrender {
    MTLPixelFormat ToMTLPixelFormat(ImageFormat format);

    /// @brief The MTLTextureUsage flags implied by an abstract ImageUsage bitmask.
    MTLTextureUsage ToMTLTextureUsage(ImageUsage usage);

    /// @brief Converts an abstract image type to its Metal texture type.
    MTLTextureType ToMTLTextureType(ImageType type);

    MTLSamplerMinMagFilter ToMTLSamplerMinMagFilter(SamplerFilter filter);

    MTLSamplerAddressMode ToMTLSamplerAddressMode(SamplerAddressMode mode);
}
#endif //RENDERING_METALUTILSCPP_HPP
