#include "LogoRenderer.h"
#include "FloopHoleAssets.h"
#include <lunasvg.h>
#include <cstdio>
#include <mutex>

namespace flbr {

LogoRaster renderLogo(int pixels) {
    if (pixels <= 0 || pixels > 1024) {
        fprintf(stderr, "FloopHole: invalid SVG logo resolution %d\n", pixels);
        return {};
    }
    static std::mutex mutex;
    std::lock_guard<std::mutex> lock(mutex);
    static auto document = lunasvg::Document::loadFromData(kFloopHoleLogo);
    if (!document) {
        fprintf(stderr, "FloopHole: cannot parse the embedded SVG logo\n");
        return {};
    }
    const auto bitmap = document->renderToBitmap(pixels, pixels);
    if (bitmap.isNull()) {
        fprintf(stderr, "FloopHole: cannot render the SVG logo at %d pixels\n", pixels);
        return {};
    }
    LogoRaster result;
    result.size = pixels;
    result.stride = bitmap.stride();
    result.pixels.assign(bitmap.data(), bitmap.data() + bitmap.stride() * bitmap.height());
    return result;
}

}
