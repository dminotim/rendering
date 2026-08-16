//
// Created by Artem Avdoshkin on 13.06.2025.
//

#ifndef RENDERING_METALUTILSCPP_HPP
#define RENDERING_METALUTILSCPP_HPP
#import <Metal/Metal.h>
#include "GImage.hpp"
#include "GSampler.hpp"
#include "PipelineState.hpp"
#import "Device.hpp"

namespace dmrender {
    MTLPixelFormat ToMTLPixelFormat(ImageFormat format);

    // --- Pipeline state conversions ---

    MTLCompareFunction ToMTLCompareFunction(CompareOp op);
    MTLStencilOperation ToMTLStencilOperation(StencilOp op);
    MTLBlendFactor ToMTLBlendFactor(BlendFactor factor);
    MTLBlendOperation ToMTLBlendOperation(BlendOp op);
    MTLColorWriteMask ToMTLColorWriteMask(ColorComponent mask);
    MTLCullMode ToMTLCullMode(CullMode mode);
    MTLWinding ToMTLWinding(FrontFace face);
    MTLTriangleFillMode ToMTLTriangleFillMode(PolygonMode mode);

    /// @return An autoreleased MTLStencilDescriptor describing @p state.
    MTLStencilDescriptor* ToMTLStencilDescriptor(const StencilOpState& state);

    /// @brief The MTLTextureUsage flags implied by an abstract ImageUsage bitmask.
    MTLTextureUsage ToMTLTextureUsage(ImageUsage usage);

    /// @brief Converts an abstract image type to its Metal texture type.
    /// @param arrayLayers Layer count; above 1 this selects the array variant of the type.
    MTLTextureType ToMTLTextureType(ImageType type, uint32_t arrayLayers = 1);

    MTLSamplerMinMagFilter ToMTLSamplerMinMagFilter(SamplerFilter filter);

    MTLSamplerAddressMode ToMTLSamplerAddressMode(SamplerAddressMode mode);
}
#endif //RENDERING_METALUTILSCPP_HPP
