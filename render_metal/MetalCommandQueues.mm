#include "MetalCommandQueues.hpp"
#include "MetalCommandbuffer.hpp"
#include "MetalDevice.hpp"

#import <Metal/Metal.h>
#import <dispatch/dispatch.h>
#import <cassert>

namespace dmrender
{
    // PIMPL data structure to hide the native Metal object from the C++ header.
    struct MetalCommandQueuesData
    {
        // The native Metal command queue object.
        // Assuming Manual Retain-Release (MRR) based on the explicit 'release' call in the destructor.
        id<MTLCommandQueue> m_commandQueue = nil;

        /// Counts frames the GPU has not finished yet. Starts full, so the first kFramesInFlight
        /// frames are recorded without blocking and only then does the CPU start waiting.
        dispatch_semaphore_t m_inFlight = nil;

        uint32_t m_currentFrameSlot = 0;
        /// True between opening a frame slot and finishing with it. See beginFrame().
        bool m_frameOpen = false;
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

        // Created at full count so the pipeline fills before the first wait blocks.
        m_data->m_inFlight = dispatch_semaphore_create(kFramesInFlight);
    }

    MetalCommandQueues::~MetalCommandQueues() {
        // Release the Metal command queue object that this class owns.
        if (m_data->m_commandQueue) {
            [m_data->m_commandQueue release];
            m_data->m_commandQueue = nullptr;
        }
        if (m_data->m_inFlight) {
            // Reclaiming every permit proves the GPU has finished every frame this queue
            // submitted, so no completion handler can still be about to signal a semaphore that
            // is being destroyed. The timeout keeps a frame that was opened and never committed —
            // an acquire that failed, an exception during recording — from hanging shutdown; the
            // permit it holds is never coming back.
            const dispatch_time_t deadline = dispatch_time(DISPATCH_TIME_NOW, 2LL * NSEC_PER_SEC);
            for (uint32_t i = 0; i < kFramesInFlight; ++i) {
                dispatch_semaphore_wait(m_data->m_inFlight, deadline);
            }
            // dispatch_release rather than -release: it is correct whether or not dispatch
            // objects are Objective-C objects on the target, which -release is not.
            dispatch_release(m_data->m_inFlight);
            m_data->m_inFlight = nil;
        }
        // The m_data (unique_ptr) is automatically destroyed here.
    }

    // --- Public API Implementation ---

    std::shared_ptr<CommandBuffer> MetalCommandQueues::getCommandBuffer() {
        // MetalCommandBuffer pulls a fresh MTLCommandBuffer out of the queue in its constructor.
        return std::make_shared<MetalCommandBuffer>(shared_from_this());
    }

    void* MetalCommandQueues::nativeHandle() const {
        // Provide access to the underlying native object for interoperability.
        return (__bridge void*)m_data->m_commandQueue;
    }

    // --- Frame pacing ---

    void MetalCommandQueues::beginFrame() {
        // Opened by the command buffer's constructor, which may happen more than once per frame;
        // only the first call should wait, or a frame recorded into two command buffers would
        // consume two of the in-flight permits and the count would drift towards a deadlock.
        if (m_data->m_frameOpen) return;

        // Blocks until the GPU has finished the frame recorded into this slot kFramesInFlight
        // ago. Everything the slot owns — the dynamic buffer regions above all — is free after.
        dispatch_semaphore_wait(m_data->m_inFlight, DISPATCH_TIME_FOREVER);

        if (auto* device = static_cast<MetalDevice*>(m_device.get())) {
            device->setCurrentFrameSlot(m_data->m_currentFrameSlot);
        }
        m_data->m_frameOpen = true;
    }

    bool MetalCommandQueues::endFrame() {
        if (!m_data->m_frameOpen) return false;

        m_data->m_frameOpen = false;
        m_data->m_currentFrameSlot = (m_data->m_currentFrameSlot + 1) % kFramesInFlight;
        if (auto* device = static_cast<MetalDevice*>(m_device.get())) {
            device->setCurrentFrameSlot(m_data->m_currentFrameSlot);
        }
        return true;
    }

    void MetalCommandQueues::abandonFrame() {
        if (!m_data->m_frameOpen) return;

        m_data->m_frameOpen = false;
        // No command buffer will carry the completion handler that would return this permit,
        // so it goes back by hand. The slot stays where it is: nothing reached the GPU.
        dispatch_semaphore_signal(m_data->m_inFlight);
    }

    uint32_t MetalCommandQueues::currentFrameSlot() const {
        return m_data->m_currentFrameSlot;
    }

    void* MetalCommandQueues::frameSemaphore() const {
        return (__bridge void*)m_data->m_inFlight;
    }

} // namespace dmrender