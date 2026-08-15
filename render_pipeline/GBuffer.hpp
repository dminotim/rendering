//
// Created by Artem Avdoshkin on 23.06.2025.
//

#ifndef RENDERING_GBUFFER_HPP
#define RENDERING_GBUFFER_HPP
#include <cstdint>  // For standard integer types like size_t, uint32_t
#include <string>
#include <string_view>

#include "Memory.hpp"

namespace dmrender {

    /**
     * @enum BufferType
     * @brief Specifies the intended use of a GBuffer.
     */
    enum class BufferType {
        Vertex,  ///< The buffer will store vertex data.
        Index,   ///< The buffer will store index data.
        Uniform  ///< The buffer will store uniform data (constants).
        // Storage, // For future use with compute shaders
    };

    /**
     * @enum BufferUsage
     * @brief Provides a performance hint about how a buffer will be accessed.
     */
    enum class BufferUsage {
        /**
         * @brief The buffer content will be set once and used many times.
         *
         * Ideal for static geometry. The backend places these in device-local memory (VRAM) and
         * uploads their contents through a staging buffer, so the GPU reads them at full speed.
         * `update()` still works but pays for a staging copy and a queue wait each time.
         *
         * @note Falls back to host-visible memory when the device-local budget cannot fit the
         *       allocation. Check `GBuffer::memoryLocation()` to see what happened.
         */
        Static,
        /**
         * @brief The buffer content will be updated frequently from the CPU.
         *
         * Ideal for per-frame uniforms. The backend places these in host-visible memory and
         * keeps them mapped, so `update()` is a plain memcpy; a staging copy per frame would
         * cost more than the slower GPU reads.
         */
        Dynamic,
        /**
         * @brief The buffer content is written once and used once (or very few times).
         *
         * Placed alongside Dynamic in host-visible memory: there is no reuse to amortise the
         * cost of a staging copy over.
         */
        Stream
    };

    /**
     * @enum IndexType
     * @brief Specifies the data type of indices in an index buffer.
     */
    enum class IndexType {
        UInt16, ///< 16-bit unsigned integer indices.
        UInt32  ///< 32-bit unsigned integer indices.
    };

    /**
     * @class GBuffer
     * @brief An abstract interface for a generic GPU buffer.
     *
     * GBuffer represents a linear block of memory on the GPU that can be used
     * to store vertex data, indices, uniforms, or other shader data.
     */
    class GBuffer {
    public:
        virtual ~GBuffer() = default;

        // Prohibit copy and move operations. Buffers are unique resources.
        GBuffer(const GBuffer&) = delete;
        GBuffer& operator=(const GBuffer&) = delete;
        GBuffer(GBuffer&&) = delete;
        GBuffer& operator=(GBuffer&&) = delete;

        /**
         * @brief Gets the type of the buffer.
         * @return The BufferType of this buffer.
         */
        virtual BufferType type() const = 0;

        /**
         * @brief Gets the usage hint of the buffer.
         * @return The BufferUsage of this buffer.
         */
        virtual BufferUsage usage() const = 0;

        /**
         * @brief Gets the total size of the buffer in bytes.
         * @return The size of the buffer.
         */
        virtual size_t size() const = 0;

        /**
         * @brief Reports where this buffer's memory actually landed.
         * @return MemoryLocation::DeviceLocal when the buffer lives in VRAM.
         * @note A BufferUsage::Static buffer normally reports DeviceLocal, but will report
         *       HostVisible if the device-local budget could not accommodate it.
         */
        virtual MemoryLocation memoryLocation() const = 0;

        /**
         * @brief Updates part or all of the buffer's content with new data from the CPU.
         *
         * Partial updates are fully supported: writing a few bytes at an offset leaves every
         * other byte in the buffer exactly as it was, whichever backend is in use and however
         * the backend chooses to multi-buffer the memory underneath.
         *
         * @param data A pointer to the new data.
         * @param dataSize The size of the new data in bytes.
         * @param offset The offset in bytes within the buffer where the update should start.
         *
         * @note On a BufferUsage::Static buffer this can be slow — the data travels through a
         *       staging buffer and the call blocks until the GPU copy completes. Buffers that
         *       change every frame should be created with BufferUsage::Dynamic, where an update
         *       is a direct write into mapped memory.
         * @note A partial update of a Dynamic buffer may cost one extra internal copy of the
         *       buffer, which is why writing the whole struct at once stays the cheapest option
         *       when the whole struct changes anyway.
         */
        virtual void update(const void* data, size_t dataSize, size_t offset = 0) = 0;

        /**
         * @brief Retrieves the native, backend-specific handle for the buffer.
         * @return A void pointer to the native object (e.g., id<MTLBuffer>, VkBuffer).
         */
        virtual void* nativeHandle() const = 0;

        /**
         * @brief Gets the debug name assigned to this buffer.
         * @return A string view of the debug name.
         */
        virtual const std::string& debugName() const = 0;

        /**
         * @brief Assigns a new debug name to this buffer.
         * @param name The new debug name.
         */
        virtual void setDebugName(const std::string& name) = 0;

    protected:
        // Protected default constructor for use by derived classes.
        GBuffer() = default;
    };

} // namespace dmrender
#endif //RENDERING_GBUFFER_HPP
