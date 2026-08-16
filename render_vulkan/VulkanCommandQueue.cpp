#include "VulkanCommandQueue.hpp"

#include <stdexcept>
#include <unordered_map>
#include <vector>

#include <render_vulkan/VulkanCommandBuffer.hpp>
#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender
{
	struct VulkanCommandQueuesData
	{
		VulkanDevice* device = nullptr;
		VkCommandPool commandPool = VK_NULL_HANDLE;
		VkQueue graphicsQueue = VK_NULL_HANDLE;
		VkQueue presentQueue = VK_NULL_HANDLE;

		std::vector<VkCommandBuffer> commandBuffers;
		std::vector<VkFence> inFlightFences;
		std::vector<VkDescriptorPool> descriptorPools;

		uint32_t currentFrameSlot = 0;
		/// True between opening a frame slot and finishing with it. See beginFrame().
		bool frameOpen = false;

		/// Cleared every time the frame slot's descriptor pool is reset.
		std::unordered_map<uint64_t, VkDescriptorSet> descriptorCache;
		uint32_t descriptorCacheHits = 0;
		uint32_t descriptorCacheRequests = 0;
	};

	namespace {
		/**
		 * @brief Distinct binding states a frame may use before the pool is exhausted.
		 *
		 * Not the number of draw calls: the command buffer caches sets by binding hash, so draws
		 * that differ only in push constants share one. What has to fit is the number of
		 * *distinct* combinations — in practice one per material.
		 *
		 * Sized for a real scene rather than a demo. San Miguel has 281 materials, which the
		 * previous ceiling of 256 would have run into partway through the first frame. Descriptor
		 * sets are cheap; the pool is a few megabytes at this size and is reset every frame.
		 */
		constexpr uint32_t kDescriptorSetsPerFrame = 4096;
	}

	VulkanCommandQueues::VulkanCommandQueues(const std::shared_ptr<Device>& device)
		: CommandQueue(device), m_data(std::make_unique<VulkanCommandQueuesData>())
	{
		m_data->device = static_cast<VulkanDevice*>(device.get());
		VkDevice logicalDevice = m_data->device->logicalDevice();

		vkGetDeviceQueue(logicalDevice, m_data->device->getGraphicsFamilyIndex(), 0, &m_data->graphicsQueue);
		vkGetDeviceQueue(logicalDevice, m_data->device->getPresentFamilyIndex(), 0, &m_data->presentQueue);

		VkCommandPoolCreateInfo poolInfo{};
		poolInfo.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
		poolInfo.queueFamilyIndex = m_data->device->getGraphicsFamilyIndex();
		poolInfo.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		VkCheck(vkCreateCommandPool(logicalDevice, &poolInfo, nullptr, &m_data->commandPool),
		        "vkCreateCommandPool");

		m_data->commandBuffers.resize(kFramesInFlight);
		VkCommandBufferAllocateInfo allocInfo{};
		allocInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
		allocInfo.commandPool = m_data->commandPool;
		allocInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocInfo.commandBufferCount = kFramesInFlight;
		VkCheck(vkAllocateCommandBuffers(logicalDevice, &allocInfo, m_data->commandBuffers.data()),
		        "vkAllocateCommandBuffers");

		// Created signalled so the very first beginFrame() does not block forever.
		VkFenceCreateInfo fenceInfo{};
		fenceInfo.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
		fenceInfo.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		m_data->inFlightFences.resize(kFramesInFlight, VK_NULL_HANDLE);
		for (uint32_t i = 0; i < kFramesInFlight; ++i) {
			VkCheck(vkCreateFence(logicalDevice, &fenceInfo, nullptr, &m_data->inFlightFences[i]),
			        "vkCreateFence");
		}

		// A draw call may consume up to two sets: one of buffers, one of textures. Each is
		// allocated against a layout declaring every slot, so the pool must be able to satisfy
		// the full layout even when only a couple of bindings are actually written.
		const VkDescriptorPoolSize poolSizes[] = {
			{ VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         kDescriptorSetsPerFrame * kMaxBindingSlots },
			{ VK_DESCRIPTOR_TYPE_STORAGE_BUFFER,         kDescriptorSetsPerFrame * kMaxBindingSlots },
			{ VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, kDescriptorSetsPerFrame * kMaxTextureSlots },
		};
		VkDescriptorPoolCreateInfo descriptorPoolInfo{};
		descriptorPoolInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
		descriptorPoolInfo.maxSets = kDescriptorSetsPerFrame * 2;
		descriptorPoolInfo.poolSizeCount = static_cast<uint32_t>(std::size(poolSizes));
		descriptorPoolInfo.pPoolSizes = poolSizes;
		m_data->descriptorPools.resize(kFramesInFlight, VK_NULL_HANDLE);
		for (uint32_t i = 0; i < kFramesInFlight; ++i) {
			VkCheck(vkCreateDescriptorPool(logicalDevice, &descriptorPoolInfo, nullptr, &m_data->descriptorPools[i]),
			        "vkCreateDescriptorPool");
		}

		m_data->device->setCurrentFrameSlot(0);
	}

	VulkanCommandQueues::~VulkanCommandQueues()
	{
		VkDevice logicalDevice = m_data->device->logicalDevice();
		vkDeviceWaitIdle(logicalDevice);

		for (VkDescriptorPool pool : m_data->descriptorPools) {
			if (pool) vkDestroyDescriptorPool(logicalDevice, pool, nullptr);
		}
		for (VkFence fence : m_data->inFlightFences) {
			if (fence) vkDestroyFence(logicalDevice, fence, nullptr);
		}
		if (m_data->commandPool) {
			// Frees the command buffers allocated from it as well.
			vkDestroyCommandPool(logicalDevice, m_data->commandPool, nullptr);
		}
	}

	std::shared_ptr<CommandBuffer> VulkanCommandQueues::getCommandBuffer()
	{
		return std::make_shared<VulkanCommandBuffer>(shared_from_this());
	}

	void VulkanCommandQueues::beginFrame()
	{
		// Two callers open the frame: acquireNextImage() and the command buffer's constructor,
		// because either can be the first thing a frame does. Only the first call should do the
		// work — repeating the resets is legal but pointless, and re-resetting a command buffer
		// that has already begun recording would be a genuine bug if the order ever changed.
		if (m_data->frameOpen) return;

		VkDevice logicalDevice = m_data->device->logicalDevice();
		const uint32_t slot = m_data->currentFrameSlot;

		// Block until the GPU is done with everything recorded into this slot two frames ago.
		// The fence is only reset right before the submit, so a frame that is opened and then
		// skipped (an acquire that fails) leaves it signalled and costs nothing next time.
		VkCheck(vkWaitForFences(logicalDevice, 1, &m_data->inFlightFences[slot], VK_TRUE, UINT64_MAX),
		        "vkWaitForFences");

		VkCheck(vkResetCommandBuffer(m_data->commandBuffers[slot], 0), "vkResetCommandBuffer");
		VkCheck(vkResetDescriptorPool(logicalDevice, m_data->descriptorPools[slot], 0),
		        "vkResetDescriptorPool");
		// Resetting the pool invalidates every set allocated from it, so the cache that points
		// at them has to go with it.
		m_data->descriptorCache.clear();
		m_data->descriptorCacheHits = 0;
		m_data->descriptorCacheRequests = 0;

		m_data->device->setCurrentFrameSlot(slot);
		m_data->frameOpen = true;
	}

	void VulkanCommandQueues::abandonFrame()
	{
		// The slot was opened but nothing was submitted into it. Reopening it must redo the
		// resets, so the flag has to come back down.
		m_data->frameOpen = false;
	}

	VkDescriptorSet VulkanCommandQueues::findCachedDescriptorSet(uint64_t key) const
	{
		++m_data->descriptorCacheRequests;
		if (auto it = m_data->descriptorCache.find(key); it != m_data->descriptorCache.end()) {
			++m_data->descriptorCacheHits;
			return it->second;
		}
		return VK_NULL_HANDLE;
	}

	void VulkanCommandQueues::cacheDescriptorSet(uint64_t key, VkDescriptorSet set)
	{
		m_data->descriptorCache.emplace(key, set);
	}

	void VulkanCommandQueues::descriptorCacheStats(uint32_t& hits, uint32_t& requests) const
	{
		hits = m_data->descriptorCacheHits;
		requests = m_data->descriptorCacheRequests;
	}

	void VulkanCommandQueues::endFrame()
	{
		m_data->frameOpen = false;
		m_data->currentFrameSlot = (m_data->currentFrameSlot + 1) % kFramesInFlight;
		m_data->device->setCurrentFrameSlot(m_data->currentFrameSlot);
	}

	uint32_t VulkanCommandQueues::currentFrameSlot() const { return m_data->currentFrameSlot; }

	VkCommandBuffer VulkanCommandQueues::currentCommandBuffer() const
	{
		return m_data->commandBuffers[m_data->currentFrameSlot];
	}

	VkFence VulkanCommandQueues::currentFence() const
	{
		return m_data->inFlightFences[m_data->currentFrameSlot];
	}

	VkDescriptorPool VulkanCommandQueues::currentDescriptorPool() const
	{
		return m_data->descriptorPools[m_data->currentFrameSlot];
	}

	VkQueue VulkanCommandQueues::graphicsQueue() const { return m_data->graphicsQueue; }
	VkQueue VulkanCommandQueues::presentQueue() const { return m_data->presentQueue; }
	VkCommandPool VulkanCommandQueues::commandPool() const { return m_data->commandPool; }

	void* VulkanCommandQueues::nativeHandle() const
	{
		return (void*)(&m_data->commandPool);
	}

	void* VulkanCommandQueues::getGraphicsQueue() const
	{
		return (void*)(&m_data->graphicsQueue);
	}

	void* VulkanCommandQueues::getPresentQueue() const
	{
		return (void*)(&m_data->presentQueue);
	}
}
