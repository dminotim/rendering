#include "VulkanPipeline.hpp"

#include <array>
#include <stdexcept>
#include <vector>

#include <render_vulkan/VulkanDevice.hpp>
#include <render_vulkan/VulkanShaderFunction.hpp>
#include <render_vulkan/VulkanUtils.hpp>

namespace dmrender {

    struct VulkanPipelineNativeData
    {
        VulkanDevice* device = nullptr;
        VkPipeline pipeline = VK_NULL_HANDLE;
        VkPipelineLayout pipelineLayout = VK_NULL_HANDLE;
        VkDescriptorSetLayout descriptorSetLayout = VK_NULL_HANDLE;
        /// Keeps the shader modules alive for as long as the pipeline object exists.
        std::shared_ptr<ShaderFunction> vertexFunction;
        std::shared_ptr<ShaderFunction> fragmentFunction;
    };

    VulkanPipeline::VulkanPipeline(const std::shared_ptr<Device>& device,
                                   const std::shared_ptr<ShaderFunction>& vertexFunction,
                                   const std::shared_ptr<ShaderFunction>& fragmentFunction,
                                   const RenderTargetFormat& targetFormat,
                                   const std::string& debugName)
        : m_data(std::make_unique<VulkanPipelineNativeData>()), m_debugName(debugName)
    {
        if (!vertexFunction || !fragmentFunction) {
            throw std::runtime_error("VulkanPipeline: both a vertex and a fragment function are required");
        }

        m_data->device = static_cast<VulkanDevice*>(device.get());
        m_data->vertexFunction = vertexFunction;
        m_data->fragmentFunction = fragmentFunction;

        VkDevice logicalDevice = m_data->device->logicalDevice();

        // --- Descriptor set 0: slot number == binding number (see the class documentation) ---
        std::vector<VkDescriptorSetLayoutBinding> bindings;
        bindings.reserve(kMaxBindingSlots);

        VkDescriptorSetLayoutBinding vertexStorage{};
        vertexStorage.binding = 0;
        vertexStorage.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        vertexStorage.descriptorCount = 1;
        vertexStorage.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;
        bindings.push_back(vertexStorage);

        for (uint32_t slot = 1; slot < kMaxBindingSlots; ++slot) {
            VkDescriptorSetLayoutBinding uniform{};
            uniform.binding = slot;
            uniform.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            uniform.descriptorCount = 1;
            uniform.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
            bindings.push_back(uniform);
        }

        VkDescriptorSetLayoutCreateInfo layoutInfo{};
        layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        layoutInfo.bindingCount = static_cast<uint32_t>(bindings.size());
        layoutInfo.pBindings = bindings.data();
        VkCheck(vkCreateDescriptorSetLayout(logicalDevice, &layoutInfo, nullptr, &m_data->descriptorSetLayout),
                "vkCreateDescriptorSetLayout");

        VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
        pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        pipelineLayoutInfo.setLayoutCount = 1;
        pipelineLayoutInfo.pSetLayouts = &m_data->descriptorSetLayout;
        VkCheck(vkCreatePipelineLayout(logicalDevice, &pipelineLayoutInfo, nullptr, &m_data->pipelineLayout),
                "vkCreatePipelineLayout");

        // --- Programmable stages ---
        auto* vs = static_cast<VulkanShaderFunction*>(vertexFunction.get());
        auto* fs = static_cast<VulkanShaderFunction*>(fragmentFunction.get());

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
        rasterizer.polygonMode = VK_POLYGON_MODE_FILL;
        rasterizer.lineWidth = 1.0f;
        // Metal rasterises with MTLCullModeNone unless told otherwise; matching that means the
        // quad's winding order cannot change the result on either backend.
        rasterizer.cullMode = VK_CULL_MODE_NONE;
        rasterizer.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rasterizer.depthBiasEnable = VK_FALSE;

        VkPipelineMultisampleStateCreateInfo multisampling{};
        multisampling.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
        multisampling.sampleShadingEnable = VK_FALSE;
        multisampling.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

        VkPipelineColorBlendAttachmentState colorBlendAttachment{};
        colorBlendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                              VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
        colorBlendAttachment.blendEnable = VK_FALSE;

        VkPipelineColorBlendStateCreateInfo colorBlending{};
        colorBlending.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
        colorBlending.logicOpEnable = VK_FALSE;
        colorBlending.attachmentCount = 1;
        colorBlending.pAttachments = &colorBlendAttachment;

        const bool hasDepth = targetFormat.depthFormat != ImageFormat::Undefined;
        VkPipelineDepthStencilStateCreateInfo depthStencil{};
        depthStencil.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
        depthStencil.depthTestEnable = hasDepth ? VK_TRUE : VK_FALSE;
        depthStencil.depthWriteEnable = hasDepth ? VK_TRUE : VK_FALSE;
        depthStencil.depthCompareOp = VK_COMPARE_OP_LESS;
        depthStencil.depthBoundsTestEnable = VK_FALSE;
        depthStencil.stencilTestEnable = VK_FALSE;

        // Only formats and sample counts matter for render pass compatibility, so the cached
        // clear-variant works for pipelines that later run inside the load-variant too.
        RenderPassKey key{};
        key.colorFormat = ToVkFormat(targetFormat.colorFormat);
        key.depthFormat = ToVkFormat(targetFormat.depthFormat);
        key.clearColor = true;
        key.clearDepth = true;
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
        if (m_data->pipelineLayout) vkDestroyPipelineLayout(logicalDevice, m_data->pipelineLayout, nullptr);
        if (m_data->descriptorSetLayout) {
            vkDestroyDescriptorSetLayout(logicalDevice, m_data->descriptorSetLayout, nullptr);
        }
    }

    void* VulkanPipeline::nativeHandle() const { return (void*)&m_data->pipeline; }

    const std::string& VulkanPipeline::debugName() const { return m_debugName; }

    VkPipeline VulkanPipeline::pipeline() const { return m_data->pipeline; }
    VkPipelineLayout VulkanPipeline::pipelineLayout() const { return m_data->pipelineLayout; }
    VkDescriptorSetLayout VulkanPipeline::descriptorSetLayout() const { return m_data->descriptorSetLayout; }

} // namespace dmrender
