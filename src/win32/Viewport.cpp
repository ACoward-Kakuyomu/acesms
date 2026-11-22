#include "win32/Viewport.h"

#include <algorithm>
#include <cstdint>

namespace smsgg::win32 {
Viewport calculateViewport(unsigned width, unsigned height, unsigned sourceWidth, unsigned sourceHeight, AspectRatio aspectRatio) {
    if (!sourceWidth || !sourceHeight) return {};
    if (aspectRatio == AspectRatio::FourThree) {
        const auto scale = std::min(width / 4, height / 3);
        if (!scale) return {};
        const auto imageWidth = scale * 4, imageHeight = scale * 3;
        return {(width - imageWidth) / 2, (height - imageHeight) / 2, imageWidth, imageHeight};
    }
    unsigned imageWidth, imageHeight;
    const auto scale = std::min(width / sourceWidth, height / sourceHeight);
    if (scale != 0) {
        imageWidth = scale * sourceWidth;
        imageHeight = scale * sourceHeight;
    } else if (uint64_t{width} * sourceHeight <= uint64_t{height} * sourceWidth) {
        imageWidth = width;
        imageHeight = static_cast<unsigned>(uint64_t{width} * sourceHeight / sourceWidth);
    } else {
        imageHeight = height;
        imageWidth = static_cast<unsigned>(uint64_t{height} * sourceWidth / sourceHeight);
    }
    if (imageWidth == 0 || imageHeight == 0) return {};
    return {(width - imageWidth) / 2, (height - imageHeight) / 2, imageWidth, imageHeight};
}
} // namespace smsgg::win32
