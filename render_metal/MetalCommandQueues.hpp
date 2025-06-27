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

        void* nativeHandle() const override;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
        std::unique_ptr<MetalCommandQueuesData> m_data;
    };
}
#endif //RENDERING_METALCOMMANDQUEUES_HPP
