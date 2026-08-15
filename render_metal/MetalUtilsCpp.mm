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

    MTLTextureType ToMTLTextureType(ImageType type) {
        switch (type) {
            case ImageType::Image1D: return MTLTextureType1D;
            case ImageType::Image3D: return MTLTextureType3D;
            case ImageType::CubeMap: return MTLTextureTypeCube;
            case ImageType::Image2D:
            default:                 return MTLTextureType2D;
        }
    }

    MTLSamplerMinMagFilter ToMTLSamplerMinMagFilter(SamplerFilter filter) {
        return (filter == SamplerFilter::Nearest) ? MTLSamplerMinMagFilterNearest
                                                  : MTLSamplerMinMagFilterLinear;
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