#pragma once

namespace smsgg::win32 {
enum class AspectRatio { SquarePixels, FourThree };
struct Viewport { unsigned x{}, y{}, width{}, height{}; };
Viewport calculateViewport(unsigned clientWidth, unsigned clientHeight, unsigned sourceWidth = 256,
    unsigned sourceHeight = 192, AspectRatio aspectRatio = AspectRatio::SquarePixels);
} // namespace smsgg::win32
