#include "MetalDevice.hpp"
#import "MetalBuffer.hpp"
#import "MetalImage.hpp"
#import "MetalSampler.hpp"

#import <Metal/Metal.h>
#import <iostream>
#import <cassert>
#import <cstring>
#import <stdexcept>

#if defined(__APPLE__) && defined(__OBJC__)

namespace dmrender
{

// PIMPL data structure to hide the native id<MTLDevice> from the C++ header.
    struct MetalDeviceNativeData
    {
        id<MTLDevice> m_device = nil;
        /// Created on first use; only resource uploads ever touch it, never the render loop.
        id<MTLCommandQueue> m_transferQueue = nil;
        /// Reused across uploads and grown on demand, mirroring the Vulkan staging buffer.
        id<MTLBuffer> m_stagingBuffer = nil;
        size_t m_stagingCapacity = 0;
        /// Which of the kFramesInFlight regions of a dynamic buffer is being written right now.
        uint32_t m_frameSlot = 0;
    };


    std::shared_ptr<Device> MetalDevice::createDefaultDevice(const std::shared_ptr<Surface>& /*surface*/) {
        id<MTLDevice> nativeDevice = MTLCreateSystemDefaultDevice();
        if (!nativeDevice) {
            std::cerr << "MetalDevice Error: Failed to create system default MTLDevice." << std::endl;
            return nullptr;
        }

        DeviceId deviceId;
        deviceId.name = std::string([[nativeDevice name] UTF8String]);
        deviceId.id = 0; // Assign a default ID.

        auto res = std::make_shared<MetalDevice>(deviceId, (void*)nativeDevice);
        return res;
    }

    std::shared_ptr<Device> MetalDevice::createDeviceById(const DeviceId& idx) {
        NSArray<id<MTLDevice>>* allDevices = MTLCopyAllDevices();
        id<MTLDevice> foundNativeDevice = nil;

        for (id<MTLDevice> device in allDevices) {
            std::string currentDeviceName = std::string([[device name] UTF8String]);
            if (currentDeviceName == idx.name) {
                // We found the device. To pass it out of this scope, we must retain it.
                foundNativeDevice = [device retain]; // Retain count is now +1 for this specific device.
                break;
            }
        }
        // Release the array we copied earlier.
        [allDevices release];

        if (!foundNativeDevice) {
            std::cerr << "MetalDevice Error: MTLDevice with name '" << idx.name << "' not found." << std::endl;
            return nullptr;
        }

        return std::make_shared<MetalDevice>(idx, (void*)[foundNativeDevice autorelease]);
    }

    std::vector<DeviceId> MetalDevice::enumerateAvailableDevices() {
        std::vector<DeviceId> availableDeviceIds;
        // MTLCopyAllDevices() returns a +1 retained array.
        NSArray<id<MTLDevice>>* allDevices = MTLCopyAllDevices();
        uint32_t currentId = 0;
        for (id<MTLDevice> device in allDevices) {
            DeviceId devId;
            devId.name = std::string([[device name] UTF8String]);
            devId.id = currentId++; // Simple incremental ID for enumeration.
            availableDeviceIds.push_back(devId);
        }
        [allDevices release];

        return availableDeviceIds;
    }


    MetalDevice::MetalDevice(DeviceId deviceId_, void* nativeDevice)
            : Device(std::move(deviceId_)), m_data(std::make_unique<MetalDeviceNativeData>())
    {
        m_data->m_device = (__bridge id<MTLDevice>)nativeDevice;

        if (m_data->m_device) {
            m_activatedInstanceExtensions.insert(DeviceExtension::Surface);
            m_activatedInstanceExtensions.insert(DeviceExtension::SwapChain);
        }
    }

    MetalDevice::~MetalDevice() {
        if (m_data->m_stagingBuffer) {
            [m_data->m_stagingBuffer release];
            m_data->m_stagingBuffer = nil;
        }
        if (m_data->m_transferQueue) {
            [m_data->m_transferQueue release];
            m_data->m_transferQueue = nil;
        }
        if (m_data->m_device) {
            [m_data->m_device release];
            m_data->m_device = nil;
        }
    }

// --- Public API Implementation ---

    bool MetalDevice::activateExtension(DeviceExtension ext) {
         return m_activatedInstanceExtensions.contains(ext);
    }

    bool MetalDevice::isExtensionAvailable(DeviceExtension ext) const {
        switch (ext) {
            case DeviceExtension::Surface:
                return m_data->m_device != nullptr;
            default:
                return false;
        }
    }

    void* MetalDevice::nativeHandle() const {
        return (__bridge void*)m_data->m_device;
    }

    std::shared_ptr<GBuffer> MetalDevice::createBuffer(
            BufferType type,
            BufferUsage usage,
            size_t size,
            const void* initialData,
            const std::string& debugName
    ) {
        assert(m_data->m_device != nullptr && "Cannot create buffer with a null native device.");
        if (!m_data->m_device) {
            std::cerr << "MetalDevice::createBuffer Error: Native device is null." << std::endl;
            return nullptr;
        }
        return std::make_shared<MetalBuffer>(this, type, usage, size, initialData, debugName);
    }

    std::shared_ptr<GImage> MetalDevice::createImage(const ImageDesc& desc, const void* initialData) {
        assert(m_data->m_device != nullptr && "Cannot create image with a null native device.");
        if (!m_data->m_device) {
            std::cerr << "MetalDevice::createImage Error: Native device is null." << std::endl;
            return nullptr;
        }
        return std::make_shared<MetalImage>(this, desc, initialData);
    }

    std::shared_ptr<GSampler> MetalDevice::createSampler(
            const SamplerDesc& desc,
            const std::string& debugName
    ) {
        assert(m_data->m_device != nullptr && "Cannot create sampler with a null native device.");
        if (!m_data->m_device) {
            std::cerr << "MetalDevice::createSampler Error: Native device is null." << std::endl;
            return nullptr;
        }
        return std::make_shared<MetalSampler>(this, desc, debugName);
    }

    MemoryBudget MetalDevice::queryMemoryBudget() const {
        MemoryBudget budget{};
        if (!m_data->m_device) {
            return budget;
        }

        // recommendedMaxWorkingSetSize is Metal's equivalent of VK_EXT_memory_budget's heapBudget:
        // how much the device would like this process to keep resident. currentAllocatedSize is
        // what this process has actually allocated, so the two together give the same picture the
        // Vulkan path reports.
        budget.deviceLocalBudgetBytes = [m_data->m_device recommendedMaxWorkingSetSize];
        budget.deviceLocalUsedBytes = [m_data->m_device currentAllocatedSize];
        budget.deviceLocalTotalBytes = budget.deviceLocalBudgetBytes;
        budget.preciseBudget = true;

        if (@available(macOS 10.15, iOS 13.0, *)) {
            budget.unifiedMemory = [m_data->m_device hasUnifiedMemory];
        } else {
            budget.unifiedMemory = false;
        }
        return budget;
    }

    SampleCount MetalDevice::maxSupportedSampleCount() const {
        if (!m_data->m_device) return SampleCount::One;

        // Metal has no limits struct to read; the device is asked about each count directly.
        for (SampleCount candidate : { SampleCount::Sixteen, SampleCount::Eight,
                                       SampleCount::Four, SampleCount::Two }) {
            if ([m_data->m_device supportsTextureSampleCount:static_cast<NSUInteger>(candidate)]) {
                return candidate;
            }
        }
        return SampleCount::One;
    }

    void MetalDevice::setCurrentFrameSlot(uint32_t slot) {
        m_data->m_frameSlot = slot % kFramesInFlight;
    }

    uint32_t MetalDevice::currentFrameSlot() const {
        return m_data->m_frameSlot;
    }

    uint32_t MetalDevice::maxSupportedAnisotropy() const {
        // Metal guarantees anisotropic filtering on every device it runs on and fixes the ceiling
        // at 16 — there is no feature flag and no limit to query, unlike Vulkan.
        return m_data->m_device ? 16u : 1u;
    }

    void MetalDevice::uploadToPrivateBuffer(void* destination,
                                            size_t destinationOffset,
                                            const void* data,
                                            size_t size) const {
        if (!destination || !data || size == 0) return;

        auto mtlDestination = (__bridge id<MTLBuffer>)destination;
        ensureTransferResources(size);

        memcpy([m_data->m_stagingBuffer contents], data, size);

        id<MTLCommandBuffer> commandBuffer = [m_data->m_transferQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromBuffer:m_data->m_stagingBuffer
                sourceOffset:0
                    toBuffer:mtlDestination
           destinationOffset:destinationOffset
                        size:size];
        [blit endEncoding];
        [commandBuffer commit];
        // Waiting keeps the staging buffer safe to overwrite on the next call and lets the caller
        // treat createBuffer() as "the data is there when this returns".
        [commandBuffer waitUntilCompleted];
    }

    void MetalDevice::uploadToPrivateTexture(void* destination,
                                             uint32_t width,
                                             uint32_t height,
                                             uint32_t depth,
                                             uint32_t mipLevel,
                                             uint32_t arrayLayer,
                                             const void* data,
                                             size_t size,
                                             size_t bytesPerRow,
                                             size_t bytesPerImage) const {
        if (!destination || !data || size == 0) return;

        auto mtlTexture = (__bridge id<MTLTexture>)destination;
        ensureTransferResources(size);

        memcpy([m_data->m_stagingBuffer contents], data, size);

        id<MTLCommandBuffer> commandBuffer = [m_data->m_transferQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromBuffer:m_data->m_stagingBuffer
                sourceOffset:0
           sourceBytesPerRow:bytesPerRow
         sourceBytesPerImage:bytesPerImage
                  sourceSize:MTLSizeMake(width, height, depth)
                   toTexture:mtlTexture
            destinationSlice:arrayLayer
            destinationLevel:mipLevel
           destinationOrigin:MTLOriginMake(0, 0, 0)];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
    }

    void MetalDevice::generateMipmapsForTexture(void* texture) const {
        if (!texture) return;

        auto mtlTexture = (__bridge id<MTLTexture>)texture;
        if (mtlTexture.mipmapLevelCount <= 1) return;

        ensureTransferResources(0);

        id<MTLCommandBuffer> commandBuffer = [m_data->m_transferQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit generateMipmapsForTexture:mtlTexture];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];
    }

    void MetalDevice::readbackFromPrivateTexture(void* source,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 uint32_t depth,
                                                 uint32_t mipLevel,
                                                 uint32_t arrayLayer,
                                                 void* destination,
                                                 size_t size,
                                                 size_t bytesPerRow,
                                                 size_t bytesPerImage) const {
        if (!source || !destination || size == 0) return;

        auto mtlSource = (__bridge id<MTLTexture>)source;
        ensureTransferResources(size);

        id<MTLCommandBuffer> commandBuffer = [m_data->m_transferQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromTexture:mtlSource
                  sourceSlice:arrayLayer
                  sourceLevel:mipLevel
                 sourceOrigin:MTLOriginMake(0, 0, 0)
                   sourceSize:MTLSizeMake(width, height, depth)
                     toBuffer:m_data->m_stagingBuffer
            destinationOffset:0
       destinationBytesPerRow:bytesPerRow
     destinationBytesPerImage:bytesPerImage];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        memcpy(destination, [m_data->m_stagingBuffer contents], size);
    }

    void MetalDevice::readbackFromPrivateBuffer(void* source,
                                                size_t sourceOffset,
                                                void* destination,
                                                size_t size) const {
        if (!source || !destination || size == 0) return;

        auto mtlSource = (__bridge id<MTLBuffer>)source;
        ensureTransferResources(size);

        id<MTLCommandBuffer> commandBuffer = [m_data->m_transferQueue commandBuffer];
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        [blit copyFromBuffer:mtlSource
                sourceOffset:sourceOffset
                    toBuffer:m_data->m_stagingBuffer
           destinationOffset:0
                        size:size];
        [blit endEncoding];
        [commandBuffer commit];
        [commandBuffer waitUntilCompleted];

        memcpy(destination, [m_data->m_stagingBuffer contents], size);
    }

    void MetalDevice::ensureTransferResources(size_t stagingSize) const {
        if (!m_data->m_transferQueue) {
            m_data->m_transferQueue = [m_data->m_device newCommandQueue];
            m_data->m_transferQueue.label = @"dmrender transfer";
        }

        if (stagingSize > m_data->m_stagingCapacity) {
            if (m_data->m_stagingBuffer) {
                [m_data->m_stagingBuffer release];
                m_data->m_stagingBuffer = nil;
            }
            m_data->m_stagingBuffer = [m_data->m_device newBufferWithLength:stagingSize
                                                                    options:MTLResourceStorageModeShared];
            if (!m_data->m_stagingBuffer) {
                throw std::runtime_error("MetalDevice: failed to allocate the staging buffer");
            }
            m_data->m_stagingBuffer.label = @"dmrender staging";
            m_data->m_stagingCapacity = stagingSize;
        }
    }

} // namespace dmrender
#endif // defined(__APPLE__) && defined(__OBJC__)