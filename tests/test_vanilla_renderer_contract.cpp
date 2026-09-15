#include <cassert>
#include <fstream>
#include <sstream>
#include <string>

static std::string read(const char* p) {
    std::ifstream in(p); assert(in.good());
    std::ostringstream s; s << in.rdbuf(); return s.str();
}
int main() {
    const auto frame=read("include/rendering/vk_frame_data.hpp");
    const auto renderer=read("src/rendering/renderer.cpp");
    const auto settings=read("src/ui/settings_panel.cpp");
    const auto terrain=read("assets/shaders/terrain.frag.glsl");
    const auto m2=read("assets/shaders/m2.frag.glsl");
    const auto wmo=read("assets/shaders/wmo.frag.glsl");
    const auto m2Loader=read("src/pipeline/m2_loader.cpp");
    const auto m2Header=read("include/rendering/m2_renderer.hpp");

    assert(frame.find("w = Vanilla 1.12 renderer") != std::string::npos);
    assert(renderer.find("classicRendering_ ? 1.0f : 0.0f") != std::string::npos);
    assert(settings.find("setClassicRendering") != std::string::npos);
    assert(terrain.find("vanillaRendering = shadowParams.w > 0.5") != std::string::npos);
    assert(m2.find("vanillaRendering = shadowParams.w > 0.5") != std::string::npos);
    assert(wmo.find("vanillaRendering = shadowParams.w > 0.5") != std::string::npos);
    assert(wmo.find("result = texColor.rgb * VertColor.rgb") != std::string::npos);

    // Build 5875 Vanilla MD20 keeps the unnamed descriptor at 0xac AFTER
    // renderFlags/texture/transparency lookups. Moving that skip earlier shifts
    // every material lookup by eight bytes and produces valid geometry with
    // the wrong materials -- most visible on trees and alpha-cutout doodads.
    const auto renderFlagsPos=m2Loader.find("header.nRenderFlags = r32()");
    const auto texLookupPos=m2Loader.find("header.nTexLookup = r32()");
    const auto texUnitPos=m2Loader.find("header.nTexUnits = r32()");
    const auto transLookupPos=m2Loader.find("header.nTransLookup = r32()");
    const auto uvAnimLookupPos=m2Loader.find("header.nUVAnimLookup = r32()");
    const auto unnamedPos=m2Loader.find("// 0xac: unnamed M2Array");
    assert(renderFlagsPos != std::string::npos);
    assert(texLookupPos > renderFlagsPos);
    assert(texUnitPos > texLookupPos);
    assert(transLookupPos > texUnitPos);
    assert(uvAnimLookupPos > transLookupPos);
    assert(unnamedPos > uvAnimLookupPos);

    // Vanilla material bit 0x04 is the only reason an M2 batch is two-sided.
    assert(m2Header.find("int32_t twoSided") != std::string::npos);
    assert(m2.find("int twoSided") != std::string::npos);
    assert(m2.find("vanillaRendering && twoSided == 0 && !gl_FrontFacing") != std::string::npos);
    return 0;
}
