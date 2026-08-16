#include "MetalSampler.hpp"
#import "MetalUtilsCpp.hpp"

#include <algorithm>

#import <Metal/Metal.h>

namespace dmrender {

    struct MetalSamplerNativeData
    {
        id<MTLSamplerState> m_sampler = nil;
    };

    MetalSampler::MetalSampler(Device* device,
                               const SamplerDesc& desc,
                               const std::string& debugName)
        : m_data(std::make_unique<MetalSamplerNativeData>()), m_desc(desc), m_debugName(debugName)
    {
        auto mtlDevice = (__bridge id<MTLDevice>)device->nativeHandle();

        MTLSamplerDescriptor* descriptor = [[MTLSamplerDescriptor alloc] init];
        descriptor.minFilter = ToMTLSamplerMinMagFilter(desc.minFilter);
        descriptor.magFilter = ToMTLSamplerMinMagFilter(desc.magFilter);
        descriptor.mipFilter = (desc.mipFilter == SamplerFilter::Nearest)
            ? MTLSamplerMipFilterNearest
            : MTLSamplerMipFilterLinear;
        descriptor.sAddressMode = ToMTLSamplerAddressMode(desc.addressU);
        descriptor.tAddressMode = ToMTLSamplerAddressMode(desc.addressV);
        descriptor.rAddressMode = ToMTLSamplerAddressMode(desc.addressW);
        descriptor.lodMinClamp = desc.minLod;
        descriptor.lodMaxClamp = desc.maxLod;

        // Metal caps anisotropy at 16 and rejects anything outside [1, 16] outright, so clamp
        // rather than pass the value through. maxSupportedAnisotropy() reports the same ceiling.
        const uint32_t requested = std::min(std::max(desc.maxAnisotropy, 1u),
                                            device->maxSupportedAnisotropy());
        descriptor.maxAnisotropy = requested;

        if (!debugName.empty()) {
            descriptor.label = [NSString stringWithUTF8String:debugName.c_str()];
        }

        // newSamplerStateWithDescriptor: returns a +1 retained object that this class owns.
        m_data->m_sampler = [mtlDevice newSamplerStateWithDescriptor:descriptor];
        [descriptor release];
    }

    MetalSampler::~MetalSampler() {
        if (m_data->m_sampler) {
            [m_data->m_sampler release];
            m_data->m_sampler = nil;
        }
    }

    void* MetalSampler::nativeHandle() const {
        return (__bridge void*)m_data->m_sampler;
    }

    const SamplerDesc& MetalSampler::desc() const {
        return m_desc;
    }

    const std::string& MetalSampler::debugName() const {
        return m_debugName;
    }

}
