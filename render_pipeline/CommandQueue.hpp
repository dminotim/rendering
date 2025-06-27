#include <utility>

//
// Created by Artem Avdoshkin on 11.06.2025.
//

#ifndef RENDERING_COMMANDLISTS_HPP
#define RENDERING_COMMANDLISTS_HPP
#include <memory>

namespace dmrender
{
    // Forward declaration to avoid including the full Device.hpp header.
    class Device;

    /**
     * @class CommandQueue
     * @brief An abstract interface for a command queue on a logical device.
     *
     * A CommandQueue is responsible for submitting command buffers to the GPU for execution.
     * It is created from a Device and represents a specific queue family (e.g., graphics, compute).
     */
    class CommandQueue
    {
    public:
        /**
         * @brief Constructs a CommandQueue for a given device.
         * @param device A shared pointer to the logical device that owns this queue.
         */
        explicit CommandQueue(const std::shared_ptr<Device>& device)
                : m_device(device) {}

        virtual ~CommandQueue() = default;

        // Prohibit copy and move operations to ensure unique ownership semantics.
        CommandQueue(const CommandQueue&) = delete;
        CommandQueue& operator=(const CommandQueue&) = delete;
        CommandQueue(CommandQueue&&) = delete;
        CommandQueue& operator=(CommandQueue&&) = delete;

        /**
         * @brief Retrieves the underlying native, backend-specific handle for the command queue.
         * @return A void pointer to the native object (e.g., id<MTLCommandQueue>, VkQueue).
         * @note Use with caution, as this breaks the abstraction layer.
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Gets the device this command queue was created from.
         * @return A shared pointer to the parent Device.
         */
        std::shared_ptr<Device> getDevice() const {
            return m_device;
        }

    protected:
        /// @brief A shared pointer to the parent logical device.
        std::shared_ptr<Device> m_device;
    };

} // namespace dmrender
#endif //RENDERING_COMMANDLISTS_HPP
