#import <Metal/Metal.h>
#include "MetalBuffer.hpp"
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

        // --- Determine the optimal Metal resource storage options based on usage hint ---
        MTLResourceOptions options;
        switch (usage) {
            case BufferUsage::Static:
                // For static data (write once, read many), 'Managed' storage is a good default.
                // It requires manual synchronization via 'didModifyRange' after CPU writes.
                options = MTLResourceStorageModeManaged;
                m_data->m_isManaged = true;
                break;
            case BufferUsage::Dynamic:
                // For frequently updated data, 'Shared' storage is simpler as it avoids manual sync,
                // but may have a performance cost. Both CPU and GPU access the same memory.
                options = MTLResourceStorageModeShared;
                m_data->m_isManaged = false;
                break;
            case BufferUsage::Stream:
                // Treat 'Stream' usage (write once, read once) as 'Dynamic' for simplicity.
                options = MTLResourceStorageModeShared;
                m_data->m_isManaged = false;
                break;
        }
        // TODO: For optimal performance with 'Static' usage, implement a path using a temporary
        // staging buffer to upload data to a 'Private' buffer, which is GPU-only.

        // --- Allocate the native MTLBuffer ---
        id<MTLBuffer> newBuffer = nil;
        if (initialData) {
            // Create a buffer and initialize it with the provided data.
            newBuffer = [mtlDeviceHandle newBufferWithBytes:initialData length:size options:options];
        } else {
            // Create a buffer without initializing its contents.
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
            // This case occurs if the buffer storage is 'Private', which is inaccessible from the CPU.
            // Updating such a buffer would require a different mechanism (e.g., a command to copy
            // from a staging buffer).
            NSLog(@"[WARNING] MetalBuffer::update failed: buffer contents are nil. This is expected for private-storage buffers.");
        }
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