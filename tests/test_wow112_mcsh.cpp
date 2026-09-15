#include <array>
#include <cassert>
#include <cstdint>

#include "pipeline/adt_shadow.hpp"

int main() {
    using namespace wowee::pipeline;
    std::array<uint8_t, kMCSHPackedBytes> packed{};
    std::array<uint8_t, kMCSHTexels> out{};

    packed[0] = 0x81; // texels 0 and 7
    packed[1] = 0x01; // texel 8
    assert(decodeMCSHShadowChannel(packed, out));
    assert(out[0] == 0x00);
    assert(out[1] == 0xff);
    assert(out[7] == 0x00);
    assert(out[8] == 0x00);
    assert(out[9] == 0xff);
    assert(out[4095] == 0xff);

    std::array<uint8_t, 1> shortInput{0xff};
    assert(!decodeMCSHShadowChannel(shortInput, out));
    for (uint8_t v : out) assert(v == 0xff);
    return 0;
}
