//
// Created by Artem Avdoshkin on 15.06.2025.
//

#ifndef RENDERING_METALUTILS_HPP
#define RENDERING_METALUTILS_HPP
#include "Device.hpp"
#import "RenderPassDescriptor.hpp"
#import "CommandQueue.hpp"
#import "Commandbuffer.hpp"
#import "SwapChain.hpp"
#include <memory>

namespace dmrender {
    bool InitImguiMetal(const std::shared_ptr<SwapChain>& swapChain);

    bool NewFrameImguiMetal(const std::shared_ptr<RenderPassDescriptor>& passDesc);

    bool RenderInternalImguiMetal(const std::shared_ptr<CommandBuffer>& cmdLists);

    bool ShutdownImguiMetal();
}
#endif //RENDERING_METALUTILS_HPP
