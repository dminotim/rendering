//
// Created by Artem Avdoshkin on 23.06.2025.
//
#ifndef RENDERING_METALBUFFER_HPP
#define RENDERING_METALBUFFER_HPP

#include "GBuffer.hpp"
#include "Device.hpp"
#include <string>
#include <memory>

namespace dmrender {
    // Forward declaration
    class Device;

    // Forward declaration for the PIMPL pattern
    struct MetalBufferData;

    /**
    * @class MetalBuffer
    * @brief Metal-specific implementation of the GBuffer interface.
    */
    class MetalBuffer : public GBuffer {
    public:
        /**
         * @brief Constructs a MetalBuffer.
         * @note This is typically called by Device::createBuffer, not directly.
         */
        MetalBuffer(const Device* device,
                    BufferType type,
                    BufferUsage usage,
                    size_t size,
                    const void* initialData,
                    const std::string& debugName);

        ~MetalBuffer() override;

        // --- GBuffer Interface Implementation ---
        BufferType type() const override;
        BufferUsage usage() const override;
        size_t size() const override;
        void update(const void* data, size_t dataSize, size_t offset) override;
        void* nativeHandle() const override;
        const std::string& debugName() const override;
        void setDebugName(const std::string& name) override;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
         std::unique_ptr<MetalBufferData> m_data;
    };

} // namespace dmrender
#endif //RENDERING_METALBUFFER_HPP
