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
    const auto m2Particles=read("src/rendering/m2_renderer_particles.cpp");
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
    assert(m2.find("vanillaRendering && alphaTest != 3 && twoSided == 0 && !gl_FrontFacing") != std::string::npos);

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
    // reference. World-doodad fade changes source/output alpha but keeps the
    // authored cutout silhouette stable.
    assert(m2.find("blendMode == 1") != std::string::npos);
    assert(m2.find("224.0 / 255.0") != std::string::npos);

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

    // M2 vertices carry full uint8 bone indices (0..255). Do not collapse
    // indices 128..255 onto bone 127; the shader already clamps them against
    // the instance's actual boneCount before indexing the palette.
    assert(m2Renderer.find("static_cast<float>(v.boneIndices[0])") != std::string::npos);
    assert(m2Renderer.find("uint8_t(127)") == std::string::npos);

    // Vanilla bone flags 0x01/0x02/0x04 rewrite the effective parent before
    // the child's TRS: translation, scale and rotation are independently
    // suppressible, while the pivot stays where the animated parent carried it
    // unless 0x01 explicitly selects the model-root origin.
    assert(m2Internal.find("const uint32_t parentArm = bone.flags & 0x07u") != std::string::npos);
    assert(m2Internal.find("case 0x02u: { // ignore parent scale") != std::string::npos);
    assert(m2Internal.find("case 0x04u: // ignore parent rotation; keep per-axis scale") != std::string::npos);
    assert(m2Internal.find("case 0x06u: // ignore parent rotation and scale") != std::string::npos);
    assert(m2Internal.find("(parentArm & 0x01u) == 0") != std::string::npos);
    assert(m2Internal.find("translation = posedPivot - basis * bone.pivot") != std::string::npos);

    // Vanilla material 0x04 is two-sided rasterization, not two-sided
    // lighting: back faces keep the exact same authored normal and diffuse
    // remains max(N.L, 0), never abs(N.L).
    assert(m2.find("!vanillaRendering && foliageTwoSided && !gl_FrontFacing") != std::string::npos);
    assert(m2.find("if (foliageTwoSided && !gl_FrontFacing) norm = -norm") == std::string::npos);
    assert(m2.find("float diff = (!vanillaRendering && foliageTwoSided)") != std::string::npos);
    assert(m2.find("float diff = foliageTwoSided ? abs(nDotL)") == std::string::npos);

    // Vanilla Model2.bls accepts authored zero normals. They remain zero so
    // directional lighting vanishes and the ambient/DC term survives.
    assert(m2.find("vec3 vanillaNormalize(vec3 v)") != std::string::npos);
    assert(m2.find("l2 > 1e-12 ? normalize(v) : vec3(0.0)") != std::string::npos);
    assert(m2.find("vanillaRendering ? vanillaNormalize(Normal) : normalize(Normal)") != std::string::npos);

    // The 1.12 skin path transforms normals with the same weighted 3x3 bone
    // rows as positions (translation excluded); do not replace it with an
    // inverse-transpose skin matrix.
    const auto m2Vert=read("assets/shaders/m2.vert.glsl");
    assert(m2Vert.find("norm = skinMat * norm") != std::string::npos);

    // Vanilla world-doodad fade uses the authored bbox centre/radius and the
    // exact FUN_00683f80 size buckets: 40→50, 100→125, 150→200; >7 never fades.
    assert(m2Header.find("authoredBoundCenter") != std::string::npos);
    assert(m2Header.find("authoredBoundRadius") != std::string::npos);
    assert(m2Renderer.find("gpuModel.authoredBoundCenter = (model.boundMin + model.boundMax) * 0.5f") != std::string::npos);
    assert(m2Renderer.find("gpuModel.authoredBoundRadius = model.boundRadius") != std::string::npos);
    assert(m2Render.find("vanillaWorldDoodadFadeAlpha") != std::string::npos);
    assert(m2Render.find("if (radius > 7.0f) return 1.0f") != std::string::npos);
    assert(m2Render.find("radius <= 0.5f") != std::string::npos);
    assert(m2Render.find("start = 40.0f") != std::string::npos);
    assert(m2Render.find("range = 10.0f") != std::string::npos);
    assert(m2Render.find("radius <= 2.5f") != std::string::npos);
    assert(m2Render.find("start = 100.0f") != std::string::npos);
    assert(m2Render.find("range = 25.0f") != std::string::npos);
    assert(m2Render.find("start = 150.0f") != std::string::npos);
    assert(m2Render.find("range = 50.0f") != std::string::npos);
    assert(m2Render.find("vanillaFadeBlend ? M2_BLEND_ALPHA") != std::string::npos);

    // Kraken's 40/80/150 mesh-profile switch stays out of Vanilla.
    assert(m2Render.find("if (!vanillaRendering_) {\n                    uint16_t desiredLOD = 0;") != std::string::npos);
    assert(m2.find("if (!vanillaRendering && blendMode <= 1) texColor.a = 1.0") != std::string::npos);

    // Vanilla detail-doodad pass: +0.25 mip bias, view-space quantised
    // 52.5→70 yd ramp, 128/255 alpha ref, two-sided, alpha blended,
    // depth-test/write ON regardless of the source M2 render flags.
    assert(m2.find("(vanillaRendering && alphaTest == 3) ? 0.25 : 0.0") != std::string::npos);
    assert(m2.find("alphaTest != 3 && twoSided == 0 && !gl_FrontFacing") != std::string::npos);
    assert(m2.find("float zEye = -(view * vec4(FragPos, 1.0)).z") != std::string::npos);
    assert(m2.find("float u = (zEye - 52.5) / 17.5") != std::string::npos);
    assert(m2.find("clamp((254.0 - 256.0 * u) / 255.0, 0.0, 252.0 / 255.0)") != std::string::npos);
    assert(m2.find("alphaCutoff = 128.0 / 255.0") != std::string::npos);
    assert(m2Render.find("(groupFading || vanillaGroundDetailCutout)") != std::string::npos);
    assert(m2Render.find("!vanillaGroundDetailCutout && (batch.materialFlags & 0x10u) != 0") != std::string::npos);
    assert(m2Render.find("!vanillaGroundDetailCutout && (batch.materialFlags & 0x08u) != 0") != std::string::npos);
    assert(m2Render.find("!(vanillaRendering_ && instance.cachedModel->isGroundDetail)") != std::string::npos);
    assert(m2Render.find("vanillaRendering_ ? 0.0f : groundDetailMaxDistance_") != std::string::npos);
    assert(m2Render.find("!vanillaRendering_ && groundDetailMaxDistance_ > 0.0f") != std::string::npos);

    // Vanilla effect trajectories use the authored emitter kernel: planes
    // spawn across their rectangle, spheres across the areaLength/areaWidth
    // shell, ranges are angles, and variation is fractional authored speed.
    assert(m2Particles.find("if (em.emitterType == 2)") != std::string::npos);
    assert(m2Particles.find("const float inner = std::min(areaLength, areaWidth)") != std::string::npos);
    assert(m2Particles.find("localPos += shell * radius") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x100u)") != std::string::npos);
    assert(m2Particles.find("areaLength * 0.5f * distN(particleRng_)") != std::string::npos);
    assert(m2Particles.find("areaWidth  * 0.5f * distN(particleRng_)") != std::string::npos);
    assert(m2Particles.find("const float polar = distN(particleRng_) * vRange") != std::string::npos);
    assert(m2Particles.find("const float azimuth = distN(particleRng_) * hRange") != std::string::npos);
    assert(m2Particles.find("speed *= 1.0f + speedVariation * distN(particleRng_)") != std::string::npos);
    assert(m2Particles.find("!vanillaRendering_ && std::abs(speed) < 0.01f") != std::string::npos);
    // Effect blend enum is not the mesh enum's mode-3 pipeline routing:
    // Vanilla particles/ribbons use alpha-weighted additive for both 3 and 4.
    assert(m2Particles.find("case 3:\n            case 4: desiredPipeline = particleAdditivePipeline_") != std::string::npos);
    assert(m2Particles.find("case 3:\n                case 4: pipe = ribbonAdditivePipeline_") != std::string::npos);
    assert(m2Particles.find("particleModulatePipeline_") != std::string::npos);
    assert(m2Particles.find("ribbonModulate2xPipeline_") != std::string::npos);

    // Classic v256 particle record uses pre-262 uint16 blend/emitter fields,
    // zSource at +0x130, and the byte-valued enabledIn track at the record tail.
    assert(m2Loader.find("EMITTER_SIZE_VANILLA = 0x1F8") != std::string::npos);
    assert(m2Loader.find("readValue<uint16_t>(m2Data, base + 0x28)") != std::string::npos);
    assert(m2Loader.find("readValue<uint16_t>(m2Data, base + 0x2A)") != std::string::npos);
    assert(m2Loader.find("parseTrackV(0xF8, em.emissionAreaLength)") != std::string::npos);
    assert(m2Loader.find("parseTrackV(0x114, em.emissionAreaWidth)") != std::string::npos);
    assert(m2Loader.find("parseTrackV(0x130, em.zSource)") != std::string::npos);
    assert(m2Loader.find("parseTrackV(0x1DC, em.visibilityTrack, TrackType::BYTE_BOOL)") != std::string::npos);
    assert(m2Particles.find("em.zSource") != std::string::npos);
    assert(m2Particles.find("em.visibilityTrack") != std::string::npos);
    assert(m2Particles.find("inst.emitterAccumulators[ei] = 0.0f") != std::string::npos);

    // Classic flipbook cells are authored in the particle record tail and
    // sampled from each particle's own normalized life, never a model clock.
    assert(m2Loader.find("base + 0x168") != std::string::npos);
    assert(m2Loader.find("base + 0x172") != std::string::npos);
    assert(m2Header.find("headCellBegin[2]") != std::string::npos);
    assert(m2Header.find("headCellRepeat[2]") != std::string::npos);
    assert(m2Particles.find("const float tLife = glm::clamp(lifeRatio") != std::string::npos);
    assert(m2Particles.find("const int authoredCell") != std::string::npos);
    assert(m2Particles.find("static_cast<uint32_t>(authoredCell) % cachedTotalTiles") != std::string::npos);

    // Build-5875 ribbon emitters are 0xE0-byte records with Classic 28-byte
    // tracks. Their texture/material fields are arrays, and the animated
    // texture-slot track selects the current direct textures[] entry.
    assert(m2Loader.find("RIBBON_SIZE_VANILLA = 0xE0") != std::string::npos);
    assert(m2Loader.find("parseRibbonTrack(0xA4, rib.textureSlotTrack, TrackType::UINT16)") != std::string::npos);
    assert(m2Loader.find("parseRibbonTrack(0xC0, rib.visibilityTrack, TrackType::BYTE_BOOL)") != std::string::npos);
    assert(m2Loader.find("rib.textureIndices = readArray<uint16_t>") != std::string::npos);
    assert(m2Loader.find("rib.materialIndices = readArray<uint16_t>") != std::string::npos);

    // Vanilla ribbons use their current animation/global-sequence tracks,
    // and an invalid/0xFFFF bone stays in model space rather than snapping to bone 0.
    assert(m2Particles.find("m2_track::sampleFloat(") != std::string::npos);
    assert(m2Particles.find("m2_track::sampleVec3(") != std::string::npos);
    assert(m2Particles.find("boneIdx = 0") == std::string::npos);

    // Vanilla ribbon history keeps the emitter bone's cross-section axis,
    // interpolates newly emitted edges between consecutive poses, clamps the
    // reference lifetime/rate, and applies age-squared gravity sag.
    assert(m2Header.find("glm::vec3 upWorld") != std::string::npos);
    assert(m2Header.find("ribbonPrevSpines") != std::string::npos);
    assert(m2Header.find("ribbonPoseValid") != std::string::npos);
    assert(m2Particles.find("std::ceil(std::max(0.0f, em.edgesPerSecond))") != std::string::npos);
    assert(m2Particles.find("std::max(0.25f, em.edgeLifetime)") != std::string::npos);
    assert(m2Particles.find("glm::mix(\n                    inst.ribbonPrevSpines[ri], spineWorld, interpolation)") != std::string::npos);
    assert(m2Particles.find("2.0f * em.gravity * ageBefore * simDt") != std::string::npos);
    assert(m2Particles.find("vanillaRendering_ ? e.upWorld") != std::string::npos);
    assert(m2Particles.find("e.age / normalizedLifetime") != std::string::npos);

    // The ground-detail opaque gate must not reference a pass-local variable
    // before it is declared; this is also the authored Vanilla cutout exception.
    assert(m2Render.find("!(vanillaRendering_ && model.isGroundDetail)") != std::string::npos);

    // World-doodad fade keeps the 224/255 AlphaKey silhouette stable: object
    // fade affects source alpha/output blend, not the texel-alpha comparison.
    assert(m2.find("float alphaForTest = texColor.a") != std::string::npos);
    assert(m2.find("alphaForTest *= vFadeAlpha") == std::string::npos);
    assert(m2.find("float outAlpha = texColor.a * vFadeAlpha") != std::string::npos);
    return 0;
}
