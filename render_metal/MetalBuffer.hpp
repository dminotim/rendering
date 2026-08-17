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
        MemoryLocation memoryLocation() const override;
        void update(const void* data, size_t dataSize, size_t offset) override;
        void readback(void* destination, size_t destinationSize, size_t offset) override;
        void* nativeHandle() const override;
        const std::string& debugName() const override;
        void setDebugName(const std::string& name) override;

        /**
         * @brief Byte offset of the region the GPU should read for the frame being recorded.
         *
         * A @c BufferUsage::Dynamic buffer holds @c kFramesInFlight copies of its contents so the
         * CPU can write next frame's values while the GPU still reads this frame's. Every place
         * that binds a buffer has to add this to its own offset, or the shader reads the copy the
         * CPU is in the middle of overwriting.
         *
         * @return 0 for static buffers, `frameSlot * regionStride` for dynamic ones.
         */
        size_t currentRegionOffset() const;

    private:
        /// @brief Pointer to implementation (PIMPL) to hide Metal-specific details.
         std::unique_ptr<MetalBufferData> m_data;
    };

} // namespace dmrender
#endif //RENDERING_METALBUFFER_HPP
