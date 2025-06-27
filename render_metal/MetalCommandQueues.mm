#include "MetalCommandQueues.hpp"

#import <Metal/Metal.h>
#import <cassert>

namespace dmrender
{
    // PIMPL data structure to hide the native Metal object from the C++ header.
    struct MetalCommandQueuesData
    {
        // The native Metal command queue object.
        // Assuming Manual Retain-Release (MRR) based on the explicit 'release' call in the destructor.
        id<MTLCommandQueue> m_commandQueue = nil;
    };

    // --- Constructor & Destructor ---

    MetalCommandQueues::MetalCommandQueues(const std::shared_ptr<Device>& device)
    // Call the base class constructor and initialize the PIMPL data structure.
            : CommandQueue(device), m_data(std::make_unique<MetalCommandQueuesData>())
    {
        // Retrieve the native MTLDevice from our abstract Device wrapper.
        auto mtlDevice = (__bridge id<MTLDevice>)m_device->nativeHandle();
        assert(mtlDevice != nil && "Cannot create a command queue from a null device.");

        // Create a new command queue from the device.
        // The 'newCommandQueue' method returns an object with a +1 retain count.
        // This class now takes ownership of the queue and is responsible for releasing it.
        m_data->m_commandQueue = [mtlDevice newCommandQueue];
    }

    MetalCommandQueues::~MetalCommandQueues() {
        // Release the Metal command queue object that this class owns.
        if (m_data->m_commandQueue) {
            [m_data->m_commandQueue release];
            m_data->m_commandQueue = nullptr;
        }
        // The m_data (unique_ptr) is automatically destroyed here.
    }

    // --- Public API Implementation ---

    void* MetalCommandQueues::nativeHandle() const {
        // Provide access to the underlying native object for interoperability.
        return (__bridge void*)m_data->m_commandQueue;
    }

} // namespace dmrender