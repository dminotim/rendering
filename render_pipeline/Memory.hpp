//
// Created by Artem Avdoshkin on 15.08.2025.
//

#ifndef RENDERING_MEMORY_HPP
#define RENDERING_MEMORY_HPP

#include <cstdint>

namespace dmrender {

    /**
     * @enum MemoryLocation
     * @brief Where a resource's memory actually ended up.
     *
     * This is a *result*, not a request. Callers describe intent through BufferUsage and the
     * backend picks the placement; querying this afterwards tells you what it decided, which
     * matters because the decision can differ between machines — a discrete GPU will put a
     * static buffer in VRAM the CPU cannot see, while an integrated one may hand back memory
     * that is both device-local and host-visible.
     */
    enum class MemoryLocation {
        /**
         * @brief GPU memory (VRAM). Fastest for the GPU to read.
         *
         * On a discrete GPU this is normally not CPU-addressable, so writing to it goes through
         * a staging buffer and a copy. Resources here are the ones that consume the VRAM budget.
         */
        DeviceLocal,

        /**
         * @brief CPU-visible memory the GPU reads over the bus.
         *
         * Used for resources the CPU rewrites every frame, where the cost of a staging copy
         * would outweigh the slower GPU reads, and as a fallback when device-local memory is
         * exhausted.
         */
        HostVisible
    };

    /**
     * @struct MemoryBudget
     * @brief A snapshot of device-local memory availability.
     *
     * Both backends can report roughly this much, though neither guarantees precision: the
     * numbers move as other processes allocate, so treat them as a guide for sizing decisions
     * rather than a contract. Take the snapshot again after a large allocation instead of
     * subtracting from a previous one.
     */
    struct MemoryBudget {
        /// @brief Total size of the device-local heap, in bytes.
        uint64_t deviceLocalTotalBytes = 0;

        /**
         * @brief How much of the heap this process may reasonably use, in bytes.
         *
         * Lower than the total, because the driver and other processes need their share.
         */
        uint64_t deviceLocalBudgetBytes = 0;

        /// @brief How much of the budget this process is currently using, in bytes.
        uint64_t deviceLocalUsedBytes = 0;

        /**
         * @brief True when the device shares one physical memory pool with the CPU.
         *
         * On such devices "uploading to VRAM" is a copy within the same RAM, so a staging path
         * buys much less — the backends still prefer device-local placement because it keeps
         * the resource in the layout and cache mode the GPU likes best.
         */
        bool unifiedMemory = false;

        /**
         * @brief True when the numbers come from a real driver budget query.
         *
         * When false, the backend could only report heap sizes and the used/budget figures are
         * estimates. On Vulkan this tracks whether VK_EXT_memory_budget was available.
         */
        bool preciseBudget = false;

        /**
         * @brief How many native memory allocations the backend currently holds.
         *
         * On Vulkan this counts VkDeviceMemory objects, which the driver caps — often at 4096 —
         * so it is the number that matters when judging whether suballocation is working. It
         * should stay small and roughly flat as resources come and go.
         *
         * Zero when the backend does not manage memory explicitly, as on Metal, where the
         * driver pools behind MTLBuffer and MTLTexture itself.
         */
        uint32_t nativeAllocationCount = 0;

        /**
         * @brief Bytes those allocations reserve, including space not yet handed out.
         *
         * The gap between this and the memory actually in use is the suballocator's headroom.
         */
        uint64_t reservedBytes = 0;

        /// @brief Bytes still available within the budget, clamped at zero.
        uint64_t availableBytes() const {
            return deviceLocalBudgetBytes > deviceLocalUsedBytes
                 ? deviceLocalBudgetBytes - deviceLocalUsedBytes
                 : 0;
        }
    };

} // namespace dmrender
#endif //RENDERING_MEMORY_HPP
