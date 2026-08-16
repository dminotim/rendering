#include "MetalUtilsCpp.hpp"
#define GLFW_EXPOSE_NATIVE_COCOA
#import "backends/imgui_impl_glfw.h"
#import "backends/imgui_impl_metal.h"

namespace dmrender {
    MTLPixelFormat ToMTLPixelFormat(ImageFormat format) {
        switch (format) {
            case ImageFormat::RGBA8_UNORM:
                return MTLPixelFormatRGBA8Unorm;
            case ImageFormat::BGRA8_UNORM:
                return MTLPixelFormatBGRA8Unorm;
            case ImageFormat::RGBA16_FLOAT:
                return MTLPixelFormatRGBA16Float;
            case ImageFormat::R32_FLOAT:
                return MTLPixelFormatR32Float;

                // Depth formats (usable for depth textures only, not CAMetalLayer)
            case ImageFormat::D32_FLOAT:
                return MTLPixelFormatDepth32Float;
            case ImageFormat::D24_UNORM_S8_UINT:
                return MTLPixelFormatDepth24Unorm_Stencil8;
            case ImageFormat::D16_UNORM:
                return MTLPixelFormatDepth16Unorm;

            // BC is available on macOS but not on Apple Silicon iOS-family GPUs, which use ASTC
            // instead. MTLPixelFormatBC* simply fails to create a texture there.
            case ImageFormat::BC1_RGBA_UNORM:
                return MTLPixelFormatBC1_RGBA;
            case ImageFormat::BC1_RGBA_SRGB:
                return MTLPixelFormatBC1_RGBA_sRGB;
            case ImageFormat::BC3_UNORM:
                return MTLPixelFormatBC3_RGBA;
            case ImageFormat::BC3_SRGB:
                return MTLPixelFormatBC3_RGBA_sRGB;
            case ImageFormat::BC4_UNORM:
                return MTLPixelFormatBC4_RUnorm;
            case ImageFormat::BC5_UNORM:
                return MTLPixelFormatBC5_RGUnorm;
            case ImageFormat::BC7_UNORM:
                return MTLPixelFormatBC7_RGBAUnorm;
            case ImageFormat::BC7_SRGB:
                return MTLPixelFormatBC7_RGBAUnorm_sRGB;

            case ImageFormat::Undefined:
            default:
                return MTLPixelFormatInvalid;
        }
    }

    MTLTextureUsage ToMTLTextureUsage(ImageUsage usage) {
        MTLTextureUsage flags = MTLTextureUsageUnknown;
        if (hasFlag(usage, ImageUsage::Sampled))      flags |= MTLTextureUsageShaderRead;
        if (hasFlag(usage, ImageUsage::Storage))      flags |= MTLTextureUsageShaderWrite;
        if (hasFlag(usage, ImageUsage::ColorTarget))  flags |= MTLTextureUsageRenderTarget;
        if (hasFlag(usage, ImageUsage::DepthStencil)) flags |= MTLTextureUsageRenderTarget;
        // TransferSrc/TransferDst need no usage flag: Metal blit encoders can copy any texture.
        return flags;
    }

    MTLTextureType ToMTLTextureType(ImageType type, uint32_t arrayLayers) {
        switch (type) {
            case ImageType::Image1D:
                return arrayLayers > 1 ? MTLTextureType1DArray : MTLTextureType1D;
            case ImageType::Image3D:
                // A 3D texture is never an array; its slices are addressed by the third
                // texture coordinate rather than by a layer index.
                return MTLTextureType3D;
            case ImageType::CubeMap:
                return MTLTextureTypeCube;
            case ImageType::Image2D:
            default:
                return arrayLayers > 1 ? MTLTextureType2DArray : MTLTextureType2D;
        }
    }

    MTLSamplerMinMagFilter ToMTLSamplerMinMagFilter(SamplerFilter filter) {
        return (filter == SamplerFilter::Nearest) ? MTLSamplerMinMagFilterNearest
                                                  : MTLSamplerMinMagFilterLinear;
    }

    MTLCompareFunction ToMTLCompareFunction(CompareOp op) {
        switch (op) {
            case CompareOp::Never:          return MTLCompareFunctionNever;
            case CompareOp::Less:           return MTLCompareFunctionLess;
            case CompareOp::Equal:          return MTLCompareFunctionEqual;
            case CompareOp::LessOrEqual:    return MTLCompareFunctionLessEqual;
            case CompareOp::Greater:        return MTLCompareFunctionGreater;
            case CompareOp::NotEqual:       return MTLCompareFunctionNotEqual;
            case CompareOp::GreaterOrEqual: return MTLCompareFunctionGreaterEqual;
            case CompareOp::Always:
            default:                        return MTLCompareFunctionAlways;
        }
    }

    MTLStencilOperation ToMTLStencilOperation(StencilOp op) {
        switch (op) {
            case StencilOp::Keep:           return MTLStencilOperationKeep;
            case StencilOp::Zero:           return MTLStencilOperationZero;
            case StencilOp::Replace:        return MTLStencilOperationReplace;
            case StencilOp::IncrementClamp: return MTLStencilOperationIncrementClamp;
            case StencilOp::DecrementClamp: return MTLStencilOperationDecrementClamp;
            case StencilOp::Invert:         return MTLStencilOperationInvert;
            case StencilOp::IncrementWrap:  return MTLStencilOperationIncrementWrap;
            case StencilOp::DecrementWrap:  return MTLStencilOperationDecrementWrap;
            default:                        return MTLStencilOperationKeep;
        }
    }

    MTLBlendFactor ToMTLBlendFactor(BlendFactor factor) {
        switch (factor) {
            case BlendFactor::Zero:             return MTLBlendFactorZero;
            case BlendFactor::One:              return MTLBlendFactorOne;
            case BlendFactor::SrcColor:         return MTLBlendFactorSourceColor;
            case BlendFactor::OneMinusSrcColor: return MTLBlendFactorOneMinusSourceColor;
            case BlendFactor::DstColor:         return MTLBlendFactorDestinationColor;
            case BlendFactor::OneMinusDstColor: return MTLBlendFactorOneMinusDestinationColor;
            case BlendFactor::SrcAlpha:         return MTLBlendFactorSourceAlpha;
            case BlendFactor::OneMinusSrcAlpha: return MTLBlendFactorOneMinusSourceAlpha;
            case BlendFactor::DstAlpha:         return MTLBlendFactorDestinationAlpha;
            case BlendFactor::OneMinusDstAlpha: return MTLBlendFactorOneMinusDestinationAlpha;
            default:                            return MTLBlendFactorOne;
        }
    }

    MTLBlendOperation ToMTLBlendOperation(BlendOp op) {
        switch (op) {
            case BlendOp::Add:             return MTLBlendOperationAdd;
            case BlendOp::Subtract:        return MTLBlendOperationSubtract;
            case BlendOp::ReverseSubtract: return MTLBlendOperationReverseSubtract;
            case BlendOp::Min:             return MTLBlendOperationMin;
            case BlendOp::Max:             return MTLBlendOperationMax;
            default:                       return MTLBlendOperationAdd;
        }
    }

    MTLColorWriteMask ToMTLColorWriteMask(ColorComponent mask) {
        MTLColorWriteMask result = MTLColorWriteMaskNone;
        if (hasFlag(mask, ColorComponent::R)) result |= MTLColorWriteMaskRed;
        if (hasFlag(mask, ColorComponent::G)) result |= MTLColorWriteMaskGreen;
        if (hasFlag(mask, ColorComponent::B)) result |= MTLColorWriteMaskBlue;
        if (hasFlag(mask, ColorComponent::A)) result |= MTLColorWriteMaskAlpha;
        return result;
    }

    MTLCullMode ToMTLCullMode(CullMode mode) {
        switch (mode) {
            case CullMode::Front: return MTLCullModeFront;
            case CullMode::Back:  return MTLCullModeBack;
            case CullMode::None:
            default:              return MTLCullModeNone;
        }
    }

    MTLWinding ToMTLWinding(FrontFace face) {
        return (face == FrontFace::Clockwise) ? MTLWindingClockwise
                                              : MTLWindingCounterClockwise;
    }

    MTLTriangleFillMode ToMTLTriangleFillMode(PolygonMode mode) {
        return (mode == PolygonMode::Line) ? MTLTriangleFillModeLines
                                           : MTLTriangleFillModeFill;
    }

    MTLStencilDescriptor* ToMTLStencilDescriptor(const StencilOpState& state) {
        MTLStencilDescriptor* descriptor = [[[MTLStencilDescriptor alloc] init] autorelease];
        descriptor.stencilFailureOperation = ToMTLStencilOperation(state.failOp);
        descriptor.depthStencilPassOperation = ToMTLStencilOperation(state.passOp);
        descriptor.depthFailureOperation = ToMTLStencilOperation(state.depthFailOp);
        descriptor.stencilCompareFunction = ToMTLCompareFunction(state.compareOp);
        descriptor.readMask = state.compareMask;
        descriptor.writeMask = state.writeMask;
        // The reference value is not part of the descriptor in Metal; MetalPipeline sets it on
        // the encoder instead.
        return descriptor;
    }

    MTLSamplerAddressMode ToMTLSamplerAddressMode(SamplerAddressMode mode) {
        switch (mode) {
            case SamplerAddressMode::Repeat:         return MTLSamplerAddressModeRepeat;
            case SamplerAddressMode::MirroredRepeat: return MTLSamplerAddressModeMirrorRepeat;
            case SamplerAddressMode::ClampToEdge:
            default:                                 return MTLSamplerAddressModeClampToEdge;
        }
    }
}