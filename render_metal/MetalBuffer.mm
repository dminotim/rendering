#import <Metal/Metal.h>
#include "MetalBuffer.hpp"
#include "MetalDevice.hpp"
#include "Device.hpp"

#include <stdexcept>
#include <cstring>
#include <cassert> // For asserting pre-conditions

namespace dmrender {

// PIMPL data structure to hide Objective-C types from the C++ header.
    struct MetalBufferData
    {
        // Use __strong with ARC or manually manage retain/release with MRR.
        // Assuming manual memory management (MRR) as per original code.
        id<MTLBuffer> m_mtlBuffer = nil;
        BufferType m_type = BufferType::Vertex;
        BufferUsage m_usage = BufferUsage::Static;
        size_t m_size = 0;
        std::string m_debugName;
        bool m_isManaged = false; // Flag to track if the buffer uses MTLResourceStorageModeManaged.
        bool m_isPrivate = false; // GPU-only storage: no `contents`, writes go through a blit.
        MemoryLocation m_location = MemoryLocation::HostVisible;
        const MetalDevice* m_device = nullptr;
    };

// --- Constructor & Destructor ---

    MetalBuffer::MetalBuffer(const Device* device,
                             BufferType type,
                             BufferUsage usage,
                             size_t size,
                             const void* initialData,
                             const std::string& debugName)
    // The PIMPL idiom: allocate the implementation data structure.
            : m_data(std::make_unique<MetalBufferData>())
    {
        // Initialize data members
        m_data->m_type = type;
        m_data->m_usage = usage;
        m_data->m_size = size;
        m_data->m_debugName = debugName;
        m_data->m_isManaged = false;

        if (!device) {
            throw std::runtime_error("MetalBuffer: Provided Device object is null.");
        }
        auto mtlDeviceHandle = (id<MTLDevice>) device->nativeHandle();

        m_data->m_device = static_cast<const MetalDevice*>(device);

        // --- Determine the optimal Metal resource storage options based on usage hint ---
        MTLResourceOptions options;
        switch (usage) {
            case BufferUsage::Static:
                // Write once, read many: worth the staging copy. 'Private' is GPU-only memory —
                // the direct equivalent of Vulkan's DEVICE_LOCAL — so the CPU cannot touch it and
                // the initial contents arrive via a blit from a shared staging buffer.
                options = MTLResourceStorageModePrivate;
                m_data->m_isPrivate = true;
                m_data->m_location = MemoryLocation::DeviceLocal;
                break;
            case BufferUsage::Dynamic:
                // Rewritten by the CPU every frame, so a staging copy per update would cost more
                // than the slower GPU reads. 'Shared' memory is visible to both.
                options = MTLResourceStorageModeShared;
                m_data->m_location = MemoryLocation::HostVisible;
                break;
            case BufferUsage::Stream:
                // Written once and read once: no reuse to amortise a staging copy over.
                options = MTLResourceStorageModeShared;
                m_data->m_location = MemoryLocation::HostVisible;
                break;
        }

        // The capacity check, matching the Vulkan path: if the allocation would not fit in what
        // the device says is still available, fall back to shared memory rather than failing.
        if (m_data->m_isPrivate) {
            const MemoryBudget budget = device->queryMemoryBudget();
            if (budget.preciseBudget && budget.availableBytes() < size) {
                options = MTLResourceStorageModeShared;
                m_data->m_isPrivate = false;
                m_data->m_location = MemoryLocation::HostVisible;
            }
        }

        // --- Allocate the native MTLBuffer ---
        id<MTLBuffer> newBuffer = nil;
        if (initialData && !m_data->m_isPrivate) {
            // Shared storage can be initialised in place.
            newBuffer = [mtlDeviceHandle newBufferWithBytes:initialData length:size options:options];
        } else {
            // Private storage has no CPU-visible contents, so it is allocated empty and filled
            // through the blit path below.
            newBuffer = [mtlDeviceHandle newBufferWithLength:size options:options];
        }

        if (!newBuffer) {
            throw std::runtime_error("Failed to create native MTLBuffer.");
        }

        // Set the debug label on the Metal object itself for easier debugging in Xcode/Instruments.
        if (!m_data->m_debugName.empty()) {
            newBuffer.label = [NSString stringWithUTF8String:m_data->m_debugName.c_str()];
        }

        // The C++ object now takes ownership of the newly created Metal object.
        // In MRR, this means we are responsible for releasing it later.
        // The 'newBuffer' has a retain count of +1 from the 'new...' methods.
        m_data->m_mtlBuffer = newBuffer;

        if (initialData && m_data->m_isPrivate) {
            m_data->m_device->uploadToPrivateBuffer((__bridge void*)newBuffer, 0, initialData, size);
        }
    }

    MetalBuffer::~MetalBuffer() {
        // Release the retained Metal object.
        if (m_data->m_mtlBuffer) {
            [m_data->m_mtlBuffer release];
            m_data->m_mtlBuffer = nil;
        }
    }

// --- GBuffer Interface Implementation ---

    BufferType MetalBuffer::type() const {
        return m_data->m_type;
    }

    BufferUsage MetalBuffer::usage() const {
        return m_data->m_usage;
    }

    size_t MetalBuffer::size() const {
        return m_data->m_size;
    }

    void MetalBuffer::update(const void* data, size_t dataSize, size_t offset) {
        if (!m_data->m_mtlBuffer) return;

        // Check for out-of-bounds access. In debug builds, this will halt execution.
        assert(offset + dataSize <= m_data->m_size && "Buffer update is out of bounds!");
        if (offset + dataSize > m_data->m_size) {
            // In release builds, log an error and return to prevent a crash.
            NSLog(@"[ERROR] MetalBuffer::update - Attempted to write past the end of the buffer.");
            return;
        }

        if (m_data->m_isPrivate) {
            // Private storage is inaccessible from the CPU, so the bytes travel through a staging
            // buffer and a blit. This blocks until the copy completes, which is why the interface
            // documents update() as slow and points frequently-updated data at BufferUsage::Dynamic.
            m_data->m_device->uploadToPrivateBuffer((__bridge void*)m_data->m_mtlBuffer,
                                                    offset, data, dataSize);
            return;
        }

        void* bufferPointer = [m_data->m_mtlBuffer contents];
        if (bufferPointer) {
            // Copy the data from the CPU pointer to the buffer's memory.
            memcpy(static_cast<char*>(bufferPointer) + offset, data, dataSize);

            // IMPORTANT: If the buffer's storage mode is 'Managed', we must explicitly notify Metal
            // that the CPU has modified this range. This ensures the changes are synchronized
            // to the GPU before it's used in a command buffer.
            if (m_data->m_isManaged) {
                [m_data->m_mtlBuffer didModifyRange:NSMakeRange(offset, dataSize)];
            }
        } else {
            NSLog(@"[ERROR] MetalBuffer::update failed: buffer contents are nil.");
        }
    }

    MemoryLocation MetalBuffer::memoryLocation() const {
        return m_data->m_location;
    }

    void* MetalBuffer::nativeHandle() const {
        return m_data->m_mtlBuffer;
    }

    const std::string& MetalBuffer::debugName() const {
        return m_data->m_debugName;
    }

    void MetalBuffer::setDebugName(const std::string& name) {
        m_data->m_debugName = name;
        if (m_data->m_mtlBuffer) {
            if (!name.empty()) {
                m_data->m_mtlBuffer.label = [NSString stringWithUTF8String:name.c_str()];
            } else {
                // It's good practice to clear the label if an empty name is provided.
                m_data->m_mtlBuffer.label = nil;
            }
        }
    }

} // namespace dmrender