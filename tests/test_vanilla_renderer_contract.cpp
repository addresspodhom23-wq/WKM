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
    const auto m2Internal=read("src/rendering/m2_renderer_internal.h");

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

    // Vanilla fog colour is blend-mode dependent: Add/AddAlpha -> black,
    // Mod -> white, Mod2x -> 128/255 grey. Unfogged still bypasses it entirely.
    assert(m2.find("blendMode == 3 || blendMode == 4") != std::string::npos);
    assert(m2.find("blendMode == 5") != std::string::npos);
    assert(m2.find("vec3(1.0)") != std::string::npos);
    assert(m2.find("blendMode == 6") != std::string::npos);
    assert(m2.find("vec3(128.0 / 255.0)") != std::string::npos);

    // Vanilla M2 depth writing is controlled by render flag 0x10, not by
    // blend mode. Transparent/additive/modulate batches write depth unless
    // their authored material explicitly disables it.
    assert(m2Header.find("noDepthWritePipelines_[7]") != std::string::npos);
    assert(m2Renderer.find("alphaPipeline_ = buildM2Pipeline(PipelineBuilder::blendAlpha(), true") != std::string::npos);
    assert(m2Renderer.find("modulate2xPipeline_ = buildM2Pipeline(PipelineBuilder::blendModulate2x(), true") != std::string::npos);
    assert(m2Render.find("batch.materialFlags & 0x10u") != std::string::npos);
    assert(m2Render.find("noDepthWritePipelines_[pipelineBlendMode]") != std::string::npos);

    // Vanilla render flag 0x08 changes the depth compare to ALWAYS without
    // implicitly changing depth writes; 0x10 remains the independent write gate.
    assert(m2Header.find("noDepthTestPipelines_[7]") != std::string::npos);
    assert(m2Header.find("noDepthTestNoWritePipelines_[7]") != std::string::npos);
    assert(m2Renderer.find("VK_COMPARE_OP_ALWAYS") != std::string::npos);
    assert(m2Render.find("batch.materialFlags & 0x08u") != std::string::npos);
    assert(m2Render.find("noDepthTestNoWritePipelines_[pipelineBlendMode]") != std::string::npos);
    assert(m2Render.find("noDepthTestPipelines_[pipelineBlendMode]") != std::string::npos);

    // Vanilla M2 AlphaKey (blend mode 1) uses the pre-Cata 224/255 alpha
    // reference and applies the instance/world fade before the comparison.
    assert(m2.find("blendMode == 1") != std::string::npos);
    assert(m2.find("224.0 / 255.0") != std::string::npos);
    assert(m2.find("alphaForTest *= vFadeAlpha") != std::string::npos);

    // Vanilla batch routing must keep the authored M2 blend mode. Kraken's
    // spell/forge promotion to additive is a non-Vanilla visual fallback only.
    assert(m2Header.find("setVanillaRendering") != std::string::npos);
    assert(renderer.find("m2Renderer->setVanillaRendering(classicRendering_)") != std::string::npos);
    assert(m2Render.find("!vanillaRendering_ && (model.isSpellEffect || fireEffectModel)") != std::string::npos);
    assert(m2Render.find("!vanillaRendering_ && (model.isSpellEffect || batch.forgeFireCard)") != std::string::npos);
    assert(m2Render.find("(batch.blendMode >= 2) || (!vanillaRendering_ && model.isSpellEffect)") != std::string::npos);

    // Ordinary Vanilla M2s must not be promoted to cutout because of decoded
    // texture alpha or black-key heuristics. Only authored blend mode 1 alpha
    // tests; the separate ground-detail pass keeps its 128/255 cutout.
    assert(m2Render.find("vanillaGroundDetailCutout") != std::string::npos);
    assert(m2Render.find("const bool krakenForceCutout =") != std::string::npos);
    assert(m2Render.find("!vanillaRendering_ &&") != std::string::npos);
    assert(m2Render.find("batch.blendMode == M2_BLEND_ALPHA_KEY ? 1 : 0") != std::string::npos);
    assert(m2.find("128.0 / 255.0") != std::string::npos);
    assert(m2.find("!vanillaRendering && colorKeyBlack != 0") != std::string::npos);

    // Vanilla glow cards/portals are authored M2 geometry. Kraken-only
    // heuristic radial sprites must neither replace nor supplement that mesh.
    assert(m2Render.find("!vanillaRendering_ && m2WantsGlowSprite(glowCard)") != std::string::npos);
    assert(m2Render.find("!vanillaRendering_ && model.isInstancePortal") != std::string::npos);
    assert(m2Render.find("!vanillaRendering_ && currentModel->isInstancePortal") != std::string::npos);
    assert(m2Render.find("!vanillaRendering_ && model.isInstancePortal) instanceFadeAlpha *= 0.72f") != std::string::npos);

    // Vanilla vegetation/cloth uses authored M2 bone animation. Kraken's
    // procedural tree wind, player brush and banner sway stay out of this path.
    assert(m2Header.find("authoredAnimationEnabled") != std::string::npos);
    assert(m2Render.find("model.isFoliageLike || model.isGroundDetail || model.isHangingCloth") != std::string::npos);
    assert(m2Render.find("if (vanillaRendering)") != std::string::npos);
    assert(m2Render.find("pc.isFoliage = 0") != std::string::npos);
    assert(m2Render.find("fillSway(pc, model, skyMode_, vanillaRendering_)") != std::string::npos);
    assert(m2Render.find("(!vanillaRendering_ && foliagePass) ? 1 : 0") != std::string::npos);

    // Vanilla billboard bones use the shared camera/view basis, not a
    // per-pivot look-at, and all four authored flag arms are recognized.
    assert(m2Internal.find("kM2BoneBillboardSpherical = 0x08") != std::string::npos);
    assert(m2Internal.find("kM2BoneBillboardLockX     = 0x10") != std::string::npos);
    assert(m2Internal.find("kM2BoneBillboardLockY     = 0x20") != std::string::npos);
    assert(m2Internal.find("kM2BoneBillboardLockZ     = 0x40") != std::string::npos);
    assert(m2Internal.find("const glm::mat3* cameraBasisWorld") != std::string::npos);
    assert(m2Internal.find("posedPivot") != std::string::npos);
    assert(m2Internal.find("glm::cross(camFwd, bz)") != std::string::npos);
    assert(renderer.find("camera->getViewMatrix())") != std::string::npos);
    return 0;
}
