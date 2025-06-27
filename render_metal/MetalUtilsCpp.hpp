//
// Created by Artem Avdoshkin on 13.06.2025.
//

#ifndef RENDERING_METALUTILSCPP_HPP
#define RENDERING_METALUTILSCPP_HPP
#import <Metal/Metal.h>
#include "GImage.hpp"
#import "Device.hpp"

namespace dmrender {
    MTLPixelFormat ToMTLPixelFormat(ImageFormat format);
}
#endif //RENDERING_METALUTILSCPP_HPP
