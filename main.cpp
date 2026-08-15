// main.cpp
//
// The entire frame lives in dmrender::run_loop(), written once against the abstract
// render_pipeline interfaces. Which backend it binds to — Metal on Apple, Vulkan elsewhere —
// is decided by CMake picking render_metal/ or render_vulkan/ and by the switches inside
// render_pipeline/RenderHelper.cpp. There is nothing platform specific left here.

#include <iostream>

#include "FastRenderer.hpp"

int main()
{
    try {
        dmrender::run_loop();
    }
    catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << std::endl;
        return 1;
    }
    return 0;
}
