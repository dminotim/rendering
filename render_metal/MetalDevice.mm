#include "MetalDevice.hpp"
#import "MetalBuffer.hpp"

#import <Metal/Metal.h>
#import <iostream>
#import <cassert>

#if defined(__APPLE__) && defined(__OBJC__)

namespace dmrender
{

// PIMPL data structure to hide the native id<MTLDevice> from the C++ header.
    struct MetalDeviceNativeData
    {
        id<MTLDevice> m_device = nil;
    };


    std::shared_ptr<Device> MetalDevice::createDefaultDevice() {
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
        }
    }

    MetalDevice::~MetalDevice() {
        if (m_data->m_device) {
            [m_data->m_device release];
            m_data->m_device = nil;
        }
    }

// --- Public API Implementation ---

    bool MetalDevice::activateExtension(DeviceExtension ext) {
        switch (ext) {
            case DeviceExtension::Surface:
                if (m_data->m_device) {
                    m_activatedInstanceExtensions.insert(ext);
                    return true;
                }
                return false;
            default:
                // Unknown or unsupported extension.
                return false;
        }
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

} // namespace dmrender
#endif // defined(__APPLE__) && defined(__OBJC__)