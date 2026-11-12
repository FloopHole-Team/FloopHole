#pragma once

#include <cstdint>
#include <vector>

namespace flbr {

struct LogoRaster {
    int size = 0;
    int stride = 0;
    std::vector<uint8_t> pixels;
};

LogoRaster renderLogo(int pixels);

}
