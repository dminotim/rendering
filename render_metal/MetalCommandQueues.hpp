//
// Created by Artem Avdoshkin on 13.06.2025.
//

#ifndef RENDERING_METALCOMMANDQUEUES_HPP
#define RENDERING_METALCOMMANDQUEUES_HPP

#include "CommandQueue.hpp"
#include "Device.hpp"

namespace dmrender {

    struct MetalCommandQueuesData;
    class MetalCommandQueues : public CommandQueue {
    public:
        explicit MetalCommandQueues(const std::shared_ptr<Device>& device);
        ~MetalCommandQueues() override;

        std::shared_ptr<CommandBuffer> getCommandBuffer() override;

        void* nativeHandle() const override;

        /**
         * @brief Opens a frame, blocking until the GPU has finished one kFramesInFlight ago.
         *
         * Metal will happily accept command buffers as fast as the CPU can build them. Without a
         * limit the CPU runs ahead until the drawable queue backs up, and by then what reaches the
         * screen was recorded several frames of input in the past — which is felt as lag rather
         * than seen as a low frame rate. Waiting here bounds that, and it is also what makes the
         * per-frame regions in MetalBuffer safe: once this returns, nothing the GPU is still
         * reading lives in the slot about to be written.
         *
         * Idempotent within a frame; only the first call does the work.
         */
        void beginFrame();

        /**
         * @brief Closes the frame and advances to the next slot.
         * @return True if this call closed an open frame, false if the frame was already closed.
         *         Only the caller that gets `true` should signal the semaphore on completion,
         *         so that one frame produces exactly one signal.
         */
        bool endFrame();

        /**
         * @brief Closes a frame that will never be submitted, returning its in-flight permit.
         *
         * The frame slot is deliberately not advanced: nothing was handed to the GPU, so the
         * next frame is free to reuse the same buffer regions. Without this, every frame that
         * acquires no drawable — a minimised window, a resize in progress — would consume a
         * permit that nothing ever signals back, and the pipeline would stall for good after
         * kFramesInFlight of them.
         */
        void abandonFrame();

        /// @brief Index of the frame slot currently being recorded, in [0, kFramesInFlight).
        uint32_t currentFrameSlot() const;

        /**
         * @brief The in-flight semaphore, as a @c dispatch_semaphore_t.
         *
         * Handed to the committed command buffer's completion handler rather than a pointer back
         * to this object: the handler runs on a driver thread at an unpredictable time, and the
         * semaphore is an Objective-C object the block retains, so it stays valid even if the
         * queue is destroyed while work is still in flight.
         */
        void* frameSemaphore() const;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalCommandQueuesData> m_data;
    };
}
#endif //RENDERING_METALCOMMANDQUEUES_HPP
