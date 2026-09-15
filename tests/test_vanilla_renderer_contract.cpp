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
    const auto m2Renderer=read("src/rendering/m2_renderer.cpp");
    const auto m2Render=read("src/rendering/m2_renderer_render.cpp");
    const auto vkPipeline=read("src/rendering/vk_pipeline.cpp");
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

    // Vanilla material bit 0x02 is Unfogged: it must survive material setup and
    // bypass world distance fog in the Vanilla renderer.
    assert(m2Header.find("int32_t unfogged") != std::string::npos);
    assert(m2Renderer.find("bgpu.materialFlags & 0x02u") != std::string::npos);
    assert(m2.find("int unfogged") != std::string::npos);
    assert(m2.find("vanillaRendering && unfogged != 0") != std::string::npos);

    // Vanilla blend mode 3 (Add) is ONE+ONE, while mode 4 (AddAlpha) is
    // SRC_ALPHA+ONE. They must not share the same Vulkan blend state.
    assert(m2Header.find("additiveOnePipeline_") != std::string::npos);
    assert(vkPipeline.find("PipelineBuilder::blendAdditiveOne()") != std::string::npos);
    assert(vkPipeline.find("srcColorBlendFactor = VK_BLEND_FACTOR_ONE") != std::string::npos);
    assert(m2Render.find("case M2_BLEND_ADD: desiredPipeline = additiveOnePipeline_") != std::string::npos);

    // Vanilla modes 5/6 are multiplicative, not additive:
    // Mod = DST_COLOR/ZERO, Mod2x = DST_COLOR/SRC_COLOR.
    assert(m2Header.find("modulatePipeline_") != std::string::npos);
    assert(m2Header.find("modulate2xPipeline_") != std::string::npos);
    assert(vkPipeline.find("PipelineBuilder::blendModulate()") != std::string::npos);
    assert(vkPipeline.find("PipelineBuilder::blendModulate2x()") != std::string::npos);
    assert(vkPipeline.find("srcColorBlendFactor = VK_BLEND_FACTOR_DST_COLOR") != std::string::npos);
    assert(vkPipeline.find("dstColorBlendFactor = VK_BLEND_FACTOR_SRC_COLOR") != std::string::npos);
    assert(m2Render.find("case M2_BLEND_MODULATE: desiredPipeline = modulatePipeline_") != std::string::npos);
    assert(m2Render.find("case M2_BLEND_MODULATE2X: desiredPipeline = modulate2xPipeline_") != std::string::npos);
    return 0;
}
