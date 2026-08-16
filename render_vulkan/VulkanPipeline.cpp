#include "VulkanPipeline.hpp"

#include <array>
#include <stdexcept>
#include <vector>

#include "Commandbuffer.hpp"   // kMaxPushConstantBytes

#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanShaderFunction.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanPipelineNativeData
    {
        VulkanDevice* device = nullptr;
        VkPipeline pipeline = VK_NULL_HANDLE;
        /// Owned by the device's layout cache — borrowed here, never destroyed here.
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout bufferSetLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout textureSetLayout = VK_NULL_HANDLE;
        /// What each buffer slot's descriptor type is, so draws can write matching descriptors.
        BufferSlotLayout bufferSlots = defaultBufferSlotLayout();
        /// Keeps the shader modules alive for as long as the pipeline object exists.
        std::shared_ptr<ShaderFunction> vertexFunction;
        std::shared_ptr<ShaderFunction> fragmentFunction;
    };

    VulkanPipeline::VulkanPipeline(const std::shared_ptr<Device>& device, const PipelineDesc& desc)
        : m_data(std::make_unique<VulkanPipelineNativeData>()), m_debugName(desc.debugName)
    {
        const RenderTargetFormat& targetFormat = desc.targetFormat;

        if (!desc.vertexFunction || !desc.fragmentFunction) {
            throw std::runtime_error("VulkanPipeline: both a vertex and a fragment function are required");
        }
        if (targetFormat.colorFormats.empty()) {
            throw std::runtime_error("VulkanPipeline: RenderTargetFormat needs at least one colour format");
        }
        if (targetFormat.colorFormats.size() > kMaxColorAttachments) {
            throw std::runtime_error("VulkanPipeline: more colour formats than kMaxColorAttachments");
        }
        if (!desc.blendStates.empty() && desc.blendStates.size() != targetFormat.colorFormats.size()) {
            throw std::runtime_error(
                "VulkanPipeline: blendStates must be empty or have one entry per colour format");
        }

        m_data->device = static_cast<VulkanDevice*>(device.get());
        m_data->vertexFunction = desc.vertexFunction;
        m_data->fragmentFunction = desc.fragmentFunction;

        VkDevice logicalDevice = m_data->device->logicalDevice();

        // Descriptor and pipeline layouts come from a device-wide cache keyed on which buffer
        // slots are storage buffers. A descriptor set layout fixes each binding's type, so the
        // arrangement has to be settled here, at pipeline creation, rather than at draw time when
        // it is finally known what got bound — by then the layout is already baked into the
        // pipeline. Declaring it in PipelineDesc is what makes that possible.
        m_data->bufferSlots = desc.bufferSlots;
        const VulkanDevice::PipelineLayoutSet& layouts =
            m_data->device->acquirePipelineLayout(desc.bufferSlots);
        m_data->bufferSetLayout = layouts.bufferSet;
        m_data->textureSetLayout = layouts.textureSet;
        m_data->pipelineLayout = layouts.pipelineLayout;

        // --- Programmable stages ---
        auto* vs = static_cast<VulkanShaderFunction*>(desc.vertexFunction.get());
        auto* fs = static_cast<VulkanShaderFunction*>(desc.fragmentFunction.get());

        std::array<VkPipelineShaderStageCreateInfo, 2> stages{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = vs->shaderModule();
        stages[0].pName = vs->entryPoint();
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = fs->shaderModule();
        stages[1].pName = fs->entryPoint();

        // No vertex bindings: geometry is pulled from a storage buffer, as Metal does.
        VkPipelineVertexInputStateCreateInfo vertexInput{};
        vertexInput.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

        VkPipelineInputAssemblyStateCreateInfo inputAssembly{};
        inputAssembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
        inputAssembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        inputAssembly.primitiveRestartEnable = VK_FALSE;

        // Viewport and scissor are dynamic; the command buffer sets them from the framebuffer,
        // which is what Metal does implicitly when a render pass begins.
        VkPipelineViewportStateCreateInfo viewportState{};
        viewportState.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
        viewportState.viewportCount = 1;
        viewportState.scissorCount = 1;

        const std::array<VkDynamicState, 2> dynamicStates = {
            VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR
        };
        VkPipelineDynamicStateCreateInfo dynamicState{};
        dynamicState.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
        dynamicState.dynamicStateCount = static_cast<uint32_t>(dynamicStates.size());
        dynamicState.pDynamicStates = dynamicStates.data();

        VkPipelineRasterizationStateCreateInfo rasterizer{};
        rasterizer.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
        rasterizer.depthClampEnable = VK_FALSE;
        rasterizer.rasterizerDiscardEnable = VK_FALSE;
        rasterizer.polygonMode = ToVkPolygonMode(desc.rasterizer.polygonMode);
        rasterizer.lineWidth = 1.0f;
        rasterizer.cullMode = ToVkCullMode(desc.rasterizer.cullMode);
        rasterizer.frontFace = ToVkFrontFace(desc.rasterizer.frontFace);
        rasterizer.depthBiasEnable = desc.rasterizer.depthBiasEnabled() ? VK_TRUE : VK_FALSE;
        rasterizer.depthBiasConstantFactor = desc.rasterizer.depthBiasConstant;
        rasterizer.depthBiasSlopeFactor = desc.rasterizer.depthBiasSlope;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        // Must equal the render pass's sample count or the pipeline is invalid there.
        multisampling.rasterizationSamples = ToVkSampleCount(targetFormat.sampleCount);

        // Vulkan requires exactly one blend state per colour attachment in the render pass, even
        // when they are all identical. An empty blendStates means "opaque everywhere".
        std::vector<VkPipelineColorBlendAttachmentState> colorBlendAttachments(
            targetFormat.colorFormats.size());
        for (size_t i = 0; i < colorBlendAttachments.size(); ++i) {
            const BlendState blend = desc.blendStates.empty() ? BlendState{} : desc.blendStates[i];
            VkPipelineColorBlendAttachmentState& attachment = colorBlendAttachments[i];

            attachment.blendEnable = blend.enabled ? VK_TRUE : VK_FALSE;
            attachment.srcColorBlendFactor = ToVkBlendFactor(blend.srcColorFactor);
            attachment.dstColorBlendFactor = ToVkBlendFactor(blend.dstColorFactor);
            attachment.colorBlendOp = ToVkBlendOp(blend.colorOp);
            attachment.srcAlphaBlendFactor = ToVkBlendFactor(blend.srcAlphaFactor);
            attachment.dstAlphaBlendFactor = ToVkBlendFactor(blend.dstAlphaFactor);
            attachment.alphaBlendOp = ToVkBlendOp(blend.alphaOp);
            attachment.colorWriteMask = ToVkColorComponents(blend.writeMask);
        }

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = static_cast<uint32_t>(colorBlendAttachments.size());
        colorBlending.pAttachments = colorBlendAttachments.data();

        const bool hasDepth = targetFormat.depthFormat != ImageFormat::Undefined;
        if (!hasDepth && (desc.depthStencil.depthTestEnabled || desc.depthStencil.stencilTestEnabled)) {
            throw std::runtime_error(
                "VulkanPipeline: depth or stencil testing was requested but "
                "RenderTargetFormat::depthFormat is Undefined");
        }

        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = desc.depthStencil.depthTestEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = desc.depthStencil.depthWriteEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = ToVkCompareOp(desc.depthStencil.depthCompareOp);
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = desc.depthStencil.stencilTestEnabled ? VK_TRUE : VK_FALSE;
        depthStencil.front = ToVkStencilOpState(desc.depthStencil.front);
        depthStencil.back = ToVkStencilOpState(desc.depthStencil.back);

        // Only attachment counts, formats and sample counts matter for render pass compatibility,
        // so the cached clear-variant works for pipelines that later run inside the load-variant,
        // and the resting layouts chosen here are irrelevant to the match.
        RenderPassKey key{};
        key.samples = ToVkSampleCount(targetFormat.sampleCount);
        const bool multisampled = targetFormat.sampleCount != SampleCount::One;
        for (ImageFormat colorFormat : targetFormat.colorFormats) {
            // Compatibility also covers whether a resolve target exists, so a multisample
            // pipeline is matched against the resolving variant of the pass.
            key.colors.push_back(RenderPassAttachmentKey{
                ToVkFormat(colorFormat), /*clear=*/true, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                /*hasResolve=*/multisampled, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL });
        }
        key.depth = RenderPassAttachmentKey{
            ToVkFormat(targetFormat.depthFormat), /*clear=*/true,
            VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL };
        const VkRenderPass renderPass = m_data->device->acquireRenderPass(key);

        VkGraphicsPipelineCreateInfo pipelineInfo{};
        pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
        pipelineInfo.stageCount = static_cast<uint32_t>(stages.size());
        pipelineInfo.pStages = stages.data();
        pipelineInfo.pVertexInputState = &vertexInput;
        pipelineInfo.pInputAssemblyState = &inputAssembly;
        pipelineInfo.pViewportState = &viewportState;
        pipelineInfo.pRasterizationState = &rasterizer;
        pipelineInfo.pMultisampleState = &multisampling;
        pipelineInfo.pDepthStencilState = &depthStencil;
        pipelineInfo.pColorBlendState = &colorBlending;
        pipelineInfo.pDynamicState = &dynamicState;
        pipelineInfo.layout = m_data->pipelineLayout;
        pipelineInfo.renderPass = renderPass;
        pipelineInfo.subpass = 0;

        VkCheck(vkCreateGraphicsPipelines(logicalDevice, VK_NULL_HANDLE, 1, &pipelineInfo, nullptr,
                                          &m_data->pipeline),
                "vkCreateGraphicsPipelines");
    }

    VulkanPipeline::~VulkanPipeline()
    {
        VkDevice logicalDevice = m_data->device->logicalDevice();
        // See VulkanBuffer's destructor: a pipeline may still be referenced by an in-flight frame.
        vkDeviceWaitIdle(logicalDevice);

        if (m_data->pipeline) vkDestroyPipeline(logicalDevice, m_data->pipeline, nullptr);
        // The layouts belong to the device's cache and outlive individual pipelines — several
        // pipelines share one set. The device destroys them.
    }

    void* VulkanPipeline::nativeHandle() const { return (void*)&m_data->pipeline; }

    const std::string& VulkanPipeline::debugName() const { return m_debugName; }

    VkPipeline VulkanPipeline::pipeline() const { return m_data->pipeline; }
    VkPipelineLayout VulkanPipeline::pipelineLayout() const { return m_data->pipelineLayout; }
    VkDescriptorSetLayout VulkanPipeline::bufferSetLayout() const { return m_data->bufferSetLayout; }
    VkDescriptorSetLayout VulkanPipeline::textureSetLayout() const { return m_data->textureSetLayout; }

    VkDescriptorType VulkanPipeline::descriptorTypeForSlot(uint32_t slot) const
    {
        if (slot >= kMaxBindingSlots) return VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        return m_data->bufferSlots[slot] == BufferBindingType::Storage
            ? VK_DESCRIPTOR_TYPE_STORAGE_BUFFER
            : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }

} // namespace dmrender
