#include "MetalUtils.hpp"
#import <Metal/Metal.h>
#import <Metal/MTLRenderPass.h>
#define GLFW_EXPOSE_NATIVE_COCOA
#import "backends/imgui_impl_glfw.h"
#import "backends/imgui_impl_metal.h"
#import "Commandbuffer.hpp"

namespace dmrender {

    bool InitImguiMetal(std::shared_ptr<Device> device) {
        auto mDevice = (id <MTLDevice>) device->nativeHandle();
        ImGui_ImplMetal_Init(mDevice);
        return true;
    }

    bool NewFrameImguiMetal(const std::shared_ptr<RenderPassDescriptor>& passDesc)
    {
        auto mtlPass =
                (MTLRenderPassDescriptor* )passDesc->nativeHandle();
        ImGui_ImplMetal_NewFrame(mtlPass);
        return true;

    }
    bool RenderInternalImguiMetal(const std::shared_ptr<CommandBuffer>& cmdLists)
    {
        auto cmdBuf =
                    (__bridge id<MTLCommandBuffer>)cmdLists->nativeHandle();
        auto enc = (__bridge id<MTLRenderCommandEncoder>)cmdLists->nativeEncoder();
        ImGui_ImplMetal_RenderDrawData(ImGui::GetDrawData(), cmdBuf, enc);
        return true;
    }

    bool ShutdownImguiMetal()
    {
        ImGui_ImplMetal_Shutdown();
        return true;
    }
}