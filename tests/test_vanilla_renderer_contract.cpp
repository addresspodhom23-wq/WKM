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
    const auto m2LoaderHeader=read("include/pipeline/m2_loader.hpp");
    const auto m2Renderer=read("src/rendering/m2_renderer.cpp");
    const auto m2Render=read("src/rendering/m2_renderer_render.cpp");
    const auto m2Particles=read("src/rendering/m2_renderer_particles.cpp");
    const auto m2ParticleVert=read("assets/shaders/m2_particle.vert.glsl");
    const auto m2ParticleFrag=read("assets/shaders/m2_particle.frag.glsl");
    const auto m2RibbonVert=read("assets/shaders/m2_ribbon.vert.glsl");
    const auto m2RibbonFrag=read("assets/shaders/m2_ribbon.frag.glsl");
    const auto vkPipeline=read("src/rendering/vk_pipeline.cpp");
    const auto m2Header=read("include/rendering/m2_renderer.hpp");
    const auto m2Internal=read("src/rendering/m2_renderer_internal.h");
    const auto m2TrackSampler=read("include/rendering/m2_track_sampler.hpp");
    const auto characterRenderer=read("src/rendering/character_renderer.cpp");
    const auto characterHeader=read("include/rendering/character_renderer.hpp");

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
    assert(renderer.find("camera->getViewMatrix(), globalTime)") != std::string::npos);

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
    assert(m2Render.find("if (!vanillaRendering_) {") != std::string::npos);
    assert(m2Render.find("uint16_t desiredLOD = 0;") != std::string::npos);
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
    assert(m2Particles.find("emissionOffset = shell * radius") != std::string::npos);
    assert(m2Particles.find("localPos += emissionOffset") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x100u)") != std::string::npos);
    assert(m2Particles.find("areaLength * 0.5f * distN(particleRng_)") != std::string::npos);
    assert(m2Particles.find("areaWidth  * 0.5f * distN(particleRng_)") != std::string::npos);
    assert(m2Particles.find("const float polar = distN(particleRng_) * vRange") != std::string::npos);
    assert(m2Particles.find("const float azimuth = distN(particleRng_) * hRange") != std::string::npos);
    assert(m2Particles.find("speed *= 1.0f + speedVariation * distN(particleRng_)") != std::string::npos);
    assert(m2Particles.find("!vanillaRendering_ && std::abs(speed) < 0.01f") != std::string::npos);
    // Effect blend enum is not the mesh enum's mode-3 pipeline routing:
    // Vanilla particles/ribbons use alpha-weighted additive for both 3 and 4.
    assert(m2Particles.find("case 3:") != std::string::npos);
    assert(m2Particles.find("case 4: desiredPipeline = particleAdditivePipeline_") != std::string::npos);
    assert(m2Particles.find("case 4: pipe = ribbonAdditivePipeline_") != std::string::npos);
    assert(m2Particles.find("particleModulatePipeline_") != std::string::npos);
    assert(m2Particles.find("ribbonModulate2xPipeline_") != std::string::npos);

    // Particle size is an authored world-space half extent. Render it as an
    // instanced camera-facing quad; Vulkan largePoints is neither required nor
    // used. Frame data is packed once into unique instance ranges before draws.
    assert(m2ParticleVert.find("gl_PointSize") == std::string::npos);
    assert(m2ParticleFrag.find("gl_PointCoord") == std::string::npos);
    assert(m2ParticleVert.find("corners[gl_VertexIndex & 3]") != std::string::npos);
    assert(m2ParticleVert.find("corner * aSize") != std::string::npos);
    assert(m2Renderer.find("pBind.inputRate = VK_VERTEX_INPUT_RATE_INSTANCE") != std::string::npos);
    assert(m2Renderer.find("VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP") != std::string::npos);
    assert(m2Renderer.find("MAX_M2_RENDER_PARTICLES * 21 * sizeof(float)") != std::string::npos);
    assert(m2Particles.find("packed + packedCount * 21") != std::string::npos);
    assert(m2Particles.find("vkCmdDraw(cmd, 4, draw.instanceCount, 0, draw.firstInstance)") != std::string::npos);
    assert(m2Particles.find("memcpy(m2ParticleVBMapped_") == std::string::npos);

    // Particle fragment output is straight-alpha. Blend/AddAlpha pipelines
    // apply SRC_ALPHA; premultiplying RGB here would apply alpha twice.
    assert(m2ParticleFrag.find("vec3 rgb = texColor.rgb * vColor.rgb;") != std::string::npos);
    assert(m2ParticleFrag.find("texColor.rgb * vColor.rgb * alpha") == std::string::npos);

    // Classic animation ranges are [first,last] inclusive. Losing the last
    // key breaks sequence-end pose/fades/visibility on every pre-WotLK track.
    assert(m2Loader.find("ranges.push_back({.start = 0, .end = disk.nTimestamps - 1})") != std::string::npos);
    assert(m2Loader.find("static_cast<size_t>(last) + 1") != std::string::npos);
    assert(m2Loader.find("const size_t end = static_cast<size_t>(keyLast) + 1") != std::string::npos);
    // Sequence-local time begins at the animation's authored start timestamp,
    // not at the first key. Otherwise a deliberately delayed first key fires
    // at t=0 and Classic animations drift from the 1.12 timeline.
    assert(m2Loader.find("hasAuthoredRanges && i < sequenceWindows.size()") != std::string::npos);
    assert(m2Loader.find("baseTime = sequenceWindows[i].first") != std::string::npos);
    assert(m2Loader.find("track.globalSequence < 0") != std::string::npos);

    // Vanilla v256 rotation keys stay one C4Quaternion (16 bytes) even when
    // the track interpolation id is cubic. Rotation-key interpolation is
    // normalized component lerp; sequence cross-fade owns the slerp.
    assert(m2Loader.find("type != TrackType::QUAT_COMPRESSED") != std::string::npos);
    assert(m2TrackSampler.find("glm::slerp") == std::string::npos);
    assert(m2TrackSampler.find("a.w + (b.w - a.w) * fraction") != std::string::npos);

    // Vanilla M2Init promotes raw file bit 0x01 to runtime blend bit 0x80,
    // then clears 0x01 for the original client's base looping animation IDs.
    assert(m2Loader.find("applyVanillaSequenceRuntimeFlags(seq)") != std::string::npos);
    assert(m2Loader.find("sequence.flags |= 0x80u") != std::string::npos);
    assert(m2Loader.find("sequence.flags &= ~0x01u") != std::string::npos);
    assert(m2Loader.find("case 223:") != std::string::npos);

    // Vanilla sequence flag 0x40 is an alias: animation data lives in the
    // aliasNext target. Resolve track sampling without overwriting the logical
    // sequence metadata, and bound the walk so corrupt/cyclic chains are safe.
    assert(m2Internal.find("(seq.flags & 0x40u) == 0") != std::string::npos);
    assert(m2Internal.find("seq.aliasNext") != std::string::npos);
    assert(m2Internal.find("hop < model.sequences.size()") != std::string::npos);
    assert(m2Internal.find("resolveM2SequenceAlias(model, instance.currentSequenceIndex)") != std::string::npos);
    assert(m2Particles.find("resolveM2SequenceAlias(gpu, inst.currentSequenceIndex)") != std::string::npos);
    assert(m2Internal.find("resolveM2SequenceAlias(model, instance.blendFromSequenceIndex)") != std::string::npos);

    // Sequence transitions use the M2Init-normalized 0x80 blend flag and
    // authored blendTime. 1.12.1 advances both clocks and applies a Hermite
    // smoothstep; quaternion cross-fade is shortest-arc slerp.
    assert(m2Header.find("blendFromSequenceIndex") != std::string::npos);
    assert(m2Internal.find("(next.flags & 0x80u) != 0") != std::string::npos);
    assert(m2Internal.find("(next.flags & 0x01u)") == std::string::npos);
    assert(m2Internal.find("next.blendTime") != std::string::npos);
    assert(m2Internal.find("(3.0f - 2.0f * t) * t * t") != std::string::npos);
    assert(m2Internal.find("glm::slerp(fromRot, rot, blendWeight)") != std::string::npos);
    assert(m2Render.find("instance.blendFromAnimTime += dtMs * instance.blendFromAnimSpeed") != std::string::npos);
    assert(m2Render.find("beginM2SequenceTransition(instance, model, newSeq)") != std::string::npos);

    // Continuous element tracks share the sequence smoothstep. Global-sequence
    // tracks bypass local cross-fade, while byte/word selectors (visibility and
    // ribbon texture slot) remain direct discrete samples.
    assert(m2Internal.find("sampleM2BlendedFloat") != std::string::npos);
    assert(m2Internal.find("sampleM2BlendedVec3") != std::string::npos);
    assert(m2Internal.find("track.globalSequence >= 0") != std::string::npos);
    assert(m2Particles.find("sampleM2BlendedFloat(gpu, inst, em.emissionRate") != std::string::npos);
    assert(m2Particles.find("em.visibilityTrack, sampleSequenceIndex") != std::string::npos);
    assert(m2Particles.find("em.textureSlotTrack, sampleSequenceIndex") != std::string::npos);
    assert(m2Render.find("sampleM2BlendedVec3(") != std::string::npos);

    // One render clock owns all Classic global-sequence phases.
    assert(m2Render.find("instance.globalSequenceTime = sharedGlobalSequenceTimeMs_") != std::string::npos);
    assert(m2Render.find("instance.globalSequenceTime += dtMs") == std::string::npos);
    assert(m2Render.find("sharedGlobalTimeSeconds * 1000.0f") != std::string::npos);
    assert(characterHeader.find("sharedGlobalSequenceTimeMs_") != std::string::npos);
    assert(characterRenderer.find("inst.globalSequenceTime = sharedGlobalSequenceTimeMs_") != std::string::npos);
    assert(characterRenderer.find("inst.globalSequenceTime += deltaTime") == std::string::npos);
    assert(renderer.find("characterRenderer->update(deltaTime, camera->getPosition(), globalTime)") != std::string::npos);

    // Particle-only tracks use the same authored sequence duration as skeletal
    // M2s. No 3333 ms/10000 ms synthetic clocks are allowed.
    assert(m2Render.find("seedInstanceTimeline(mdlRef, instance)") != std::string::npos);
    assert(m2Render.find("seedInstanceTimeline(mdl2, instance)") != std::string::npos);
    assert(m2Render.find("const float duration = instance.animDuration") != std::string::npos);
    assert(m2Render.find("kParticleWrapMs") == std::string::npos);
    assert(m2Render.find("randFloat(0.0f, 10000.0f)") == std::string::npos);
    assert(m2Render.find("M2_DEFAULT_PARTICLE_ANIM_MS") == std::string::npos);

    // Vanilla point sprites keep the authored square billboard; the BLP alpha,
    // not Kraken's synthetic radial mask, defines their silhouette.
    assert(m2ParticleFrag.find("push.vanillaRendering != 0") != std::string::npos);
    assert(m2ParticleFrag.find("? 1.0") != std::string::npos);
    assert(m2Particles.find(".vanillaRendering = vanillaRendering_ ? 1 : 0") != std::string::npos);
    assert(m2Renderer.find("pushRange.size = 20") != std::string::npos);

    // Vanilla model-space emitters (flag 0x10) keep particle state local,
    // apply the emitter kernel's +90deg local-Z rotation, and re-project through
    // the live bone every draw. Classic drag is the plain f32 at +0x194.
    assert(m2Loader.find("base + 0x194") != std::string::npos);
    assert(m2LoaderHeader.find("float drag = 0.0f") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x10u) != 0") != std::string::npos);
    assert(m2Particles.find("return glm::vec3(-v.y, v.x, v.z)") != std::string::npos);
    assert(m2Particles.find("p.position = localPos") != std::string::npos);
    assert(m2Particles.find("p.velocity = dir * speed") != std::string::npos);
    assert(m2Particles.find("inst.modelMatrix * liveBone * glm::vec4(p.position, 1.0f)") != std::string::npos);
    assert(m2Particles.find("std::min(simDt * em.drag, 1.0f)") != std::string::npos);

    // Classic particle simulation advances the existing pool before births
    // and clamps emission/integration/follow/inherit to a 0.1 s step.
    const auto particleUpdatePos = m2Render.find("updateParticles(instance, deltaTime)");
    const auto particleEmitPos = m2Render.find("emitParticles(instance, *instance.cachedModel, deltaTime)");
    assert(particleUpdatePos != std::string::npos);
    assert(particleEmitPos != std::string::npos);
    assert(particleUpdatePos < particleEmitPos);
    assert(m2Particles.find("std::min(std::max(dt, 0.0f), 0.1f)") != std::string::npos);
    assert(m2Particles.find("rate * simDt") != std::string::npos);
    assert(m2Particles.find("p.life += simDt") != std::string::npos);

    // Classic 0x4000 follow uses the authored speed->scale line, while 0x40
    // samples inherited emitter velocity at a strict 30 Hz sample-and-hold.
    assert(m2Loader.find("base + 0x190") != std::string::npos);
    assert(m2Loader.find("base + 0x1C4") != std::string::npos);
    assert(m2Loader.find("base + 0x1D0") != std::string::npos);
    assert(m2LoaderHeader.find("float inheritScale = 0.0f") != std::string::npos);
    assert(m2LoaderHeader.find("float followSpeed1 = 0.0f") != std::string::npos);
    assert(m2Header.find("particleEmitterPrevOrigins") != std::string::npos);
    assert(m2Header.find("particleInheritVelocities") != std::string::npos);
    assert(m2Particles.find("(pem.flags & 0x4000u) != 0") != std::string::npos);
    assert(m2Particles.find("(pem.followScale2 - pem.followScale1)") != std::string::npos);
    assert(m2Particles.find("modelSpace ? fraction - 1.0f : fraction") != std::string::npos);
    assert(m2Particles.find("constexpr float kInheritInterval = 1.0f / 30.0f") != std::string::npos);
    assert(m2Particles.find("(pem.flags & 0x40u) != 0") != std::string::npos);
    assert(m2Particles.find("kInheritInterval / accumulator") != std::string::npos);
    assert(m2Particles.find("inheritFactor * inherited") != std::string::npos);
    assert(m2Render.find("particleEmitterOriginValid.begin()") != std::string::npos);

    // Vanilla particle draw ordering must not inherit unordered_map order.
    // Each authored emitter/pool gets a stable order token; packing follows
    // that order so alpha ranges cannot cross another emitter barrier.
    assert(m2Header.find("uint64_t orderToken = 0") != std::string::npos);
    assert(m2Header.find("uint64_t submissionOrder") != std::string::npos);
    assert(m2Particles.find("particleInstanceOrder") != std::string::npos);
    assert(m2Particles.find("recursiveInstanceOrder") != std::string::npos);
    assert(m2Particles.find("static_cast<uint64_t>(p.emitterIndex) * 8u") != std::string::npos);
    assert(m2Particles.find("recursive.parentEmitterIndex) * 8u") != std::string::npos);
    assert(m2Particles.find("std::vector<ParticleGroup*> frameGroups") != std::string::npos);
    assert(m2Particles.find("lhs->submissionOrder <") != std::string::npos);
    assert(m2Particles.find("for (ParticleGroup* groupPtr : frameGroups)") != std::string::npos);

    // Classic 0x08 fog policy and 0x1000 emitter-plane heads.
    assert(m2Header.find("uint8_t fogPolicy = 1") != std::string::npos);
    assert(m2Particles.find("(cachedEm->flags & 0x08u) != 0") != std::string::npos);
    assert(m2Particles.find("cachedBlendType == 3 || cachedBlendType == 4") != std::string::npos);
    assert(m2Particles.find("(cachedEm->flags & 0x1000u) != 0") != std::string::npos);
    assert(m2Particles.find("glm::vec3(0.0f, 1.0f, 0.0f)") != std::string::npos);
    assert(m2Particles.find("glm::vec3(-1.0f, 0.0f, 0.0f)") != std::string::npos);
    assert(m2Particles.find("cachedPlaneRight") != std::string::npos);
    assert(m2Particles.find(".fogPolicy = static_cast<int>(group.fogPolicy)") != std::string::npos);
    assert(m2Renderer.find("pushRange.size = 20") != std::string::npos);
    assert(m2Renderer.find("pBind.stride = 21 * sizeof(float)") != std::string::npos);
    assert(m2ParticleVert.find("layout(location = 8) in vec3 aPlaneRight") != std::string::npos);
    assert(m2ParticleVert.find("layout(location = 9) in vec3 aPlaneUp") != std::string::npos);
    assert(m2ParticleVert.find("Classic flag 0x1000") != std::string::npos);
    assert(m2ParticleFrag.find("int fogPolicy") != std::string::npos);
    assert(m2ParticleFrag.find("push.fogPolicy == 2 ? vec3(0.0) : fogColor.rgb") != std::string::npos);
    assert(m2ParticleFrag.find("alpha *= vFogVisibility") != std::string::npos);
    assert(m2ParticleFrag.find("if (push.vanillaRendering != 0)") != std::string::npos);
    assert(m2ParticleFrag.find("float alpha = texColor.a * vColor.a * edge;") != std::string::npos);

    // Classic RecursionModel is a CPU-only child-emitter definition. Only
    // the first four usable child records are wired; no .skin is required.
    assert(m2Header.find("struct ParticleRecursionRuntime") != std::string::npos);
    assert(m2Header.find("std::vector<M2RecursiveParticle> recursiveParticles") != std::string::npos);
    assert(m2Header.find("std::vector<M2RecursiveEmitterState> recursiveEmitterStates") != std::string::npos);
    assert(m2Renderer.find("requestedRecursion") != std::string::npos);
    assert(m2Renderer.find("std::min<size_t>(4, runtime.model.particleEmitters.size())") != std::string::npos);
    assert(m2Renderer.find("runtime.validEmitterIndices.push_back") != std::string::npos);
    assert(m2Renderer.find("particleRecursionModels_.emplace") != std::string::npos);
    assert(m2Renderer.find("M2 particle recursion has no usable child emitters") != std::string::npos);
    assert(m2Particles.find("void M2Renderer::updateRecursiveParticles") != std::string::npos);
    assert(m2Particles.find("parentParticle.position + local.offset") != std::string::npos);
    assert(m2Particles.find("parentLinear * local.offset") != std::string::npos);
    assert(m2Particles.find("inheritedFactor *") != std::string::npos);
    assert(m2Particles.find("parentParticle.velocity") != std::string::npos);
    assert(m2Particles.find("particle.orientation =") != std::string::npos);
    assert(m2Particles.find("glm::quat(1.0f") != std::string::npos);
    assert(m2Particles.find("recursive.particle;") != std::string::npos);
    assert(m2Particles.find("runtime.emitterTextures") != std::string::npos);
    assert(m2Particles.find("recursive.parentEmitterIndex") != std::string::npos);
    assert(m2Particles.find("em.headOrTail >= 1") != std::string::npos);
    assert(m2Render.find("updateRecursiveParticles(instance, *instance.cachedModel, deltaTime)") != std::string::npos);
    const auto recursionEmitPos = m2Render.find("emitParticles(instance, *instance.cachedModel, deltaTime)");
    const auto recursionUpdatePos = m2Render.find("updateRecursiveParticles(instance, *instance.cachedModel, deltaTime)");
    assert(recursionEmitPos != std::string::npos && recursionUpdatePos != std::string::npos);
    assert(recursionEmitPos < recursionUpdatePos);

    // Classic particle geometry models are M2Array strings in the emitter
    // prefix. Their tumble range is retained, integrated per particle, and the
    // geometry itself is drawn through the ordinary M2 material/depth path.
    assert(m2LoaderHeader.find("std::string geometryModel") != std::string::npos);
    assert(m2LoaderHeader.find("std::string recursionModel") != std::string::npos);
    assert(m2Loader.find("readParticleModelName(0x18)") != std::string::npos);
    assert(m2Loader.find("readParticleModelName(0x20)") != std::string::npos);
    assert(m2Loader.find("base + 0x19C") != std::string::npos);
    assert(m2Loader.find("base + 0x1B0") != std::string::npos);
    assert(m2LoaderHeader.find("angularVelocityMin") != std::string::npos);
    assert(m2Header.find("glm::quat orientation") != std::string::npos);
    assert(m2Header.find("glm::vec3 angularVelocity") != std::string::npos);
    assert(m2Particles.find("em.angularVelocityMax - em.angularVelocityMin") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x200u) != 0") != std::string::npos);
    assert(m2Particles.find("p.orientation = glm::normalize(p.orientation * delta)") != std::string::npos);
    assert(m2Particles.find("!em.geometryModel.empty()") != std::string::npos);
    assert(m2Renderer.find("ensureParticleGeometryModelsLoaded") != std::string::npos);
    assert(m2Renderer.find("pipeline::skinPathForM2(path)") != std::string::npos);
    assert(m2Render.find("ensureParticleGeometryModelsLoaded()") != std::string::npos);
    assert(m2Render.find("drawParticleGeometry(false)") != std::string::npos);
    assert(m2Render.find("drawParticleGeometry(true)") != std::string::npos);
    assert(m2Render.find("particleGeometryModel(emitter.geometryModel)") != std::string::npos);
    assert(m2Render.find("gpuInst.instanceColor = glm::vec4(color, alpha)") != std::string::npos);
    assert(m2Render.find("batch.materialFlags & 0x10u") != std::string::npos);
    assert(m2Render.find("batch.materialFlags & 0x08u") != std::string::npos);
    assert(m2Render.find("kMaxGeometryParticlesPerEmitter = 128") != std::string::npos);

    // Classic spline emitters carry a cubic-Bezier chain at +0x1D4/+0x1D8.
    // areaLength/areaWidth choose a normalized interval along arc length.
    assert(m2Loader.find("base + 0x1D4") != std::string::npos);
    assert(m2Loader.find("base + 0x1D8") != std::string::npos);
    assert(m2LoaderHeader.find("splinePoints") != std::string::npos);
    assert(m2Particles.find("sampleClassicSpline") != std::string::npos);
    assert(m2Particles.find("cubicBezierTangent") != std::string::npos);
    assert(m2Particles.find("rotateAroundAxis") != std::string::npos);
    assert(m2Particles.find("splineStart + dist01(particleRng_)") != std::string::npos);

    // Burst flag 0x8000 fires rate-as-count only on the gate's rising edge.
    // Sphere flag 0x80 kills an inward stream once it crosses the birth centre.
    assert(m2Header.find("particleEmitterGatePrev") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x8000u) != 0") != std::string::npos);
    assert(m2Particles.find("std::floor(std::max(0.0f, rate))") != std::string::npos);
    assert(m2Header.find("glm::vec3 emitterOrigin") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x80u) != 0") != std::string::npos);
    assert(m2Particles.find("p.position - p.emitterOrigin") != std::string::npos);

    // Classic head/tail routing: 1 is tail-only, >=2 draws both. Tails use
    // the authored tail-cell ramp and extend opposite current velocity for
    // TailTime seconds; flag 0x400 clamps the streak to current particle age.
    assert(m2Loader.find("base + 0x2C") != std::string::npos);
    assert(m2Loader.find("base + 0x174") != std::string::npos);
    assert(m2Loader.find("base + 0x17C") != std::string::npos);
    assert(m2LoaderHeader.find("uint8_t headOrTail = 0") != std::string::npos);
    assert(m2LoaderHeader.find("float tailTime = 0.0f") != std::string::npos);
    assert(m2Particles.find("em.headOrTail != 1") != std::string::npos);
    assert(m2Particles.find("em.headOrTail >= 1") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x400u) != 0") != std::string::npos);
    assert(m2Particles.find("sampleCell(em.tailCellBegin, em.tailCellEnd)") != std::string::npos);
    assert(m2ParticleVert.find("-aVelocity * max(aTailSeconds, 0.0)") != std::string::npos);
    assert(m2ParticleVert.find("projectedLen2 < 7.7e-4") != std::string::npos);
    assert(m2Renderer.find("pBind.stride = 21 * sizeof(float)") != std::string::npos);
    assert(m2Renderer.find("MAX_M2_RENDER_PARTICLES * 21 * sizeof(float)") != std::string::npos);

    // Classic twinkle is a per-particle deterministic LUT phase and can
    // hard-gate a quad; authored head spin rotates the camera billboard. Placement
    // scale affects particle half-size only under emitter flag 0x20.
    assert(m2Loader.find("base + 0x180") != std::string::npos);
    assert(m2Loader.find("base + 0x198") != std::string::npos);
    assert(m2LoaderHeader.find("twinkleSpeed") != std::string::npos);
    assert(m2LoaderHeader.find("float spin = 0.0f") != std::string::npos);
    assert(m2Particles.find("vanillaTwinkleLut") != std::string::npos);
    assert(m2Particles.find("p.phase = particleRng_() & 0x7Fu") != std::string::npos);
    assert(m2Particles.find("twinkleNoise > em.twinklePercent") != std::string::npos);
    assert(m2Particles.find("em.twinkleMax - em.twinkleMin") != std::string::npos);
    assert(m2Particles.find("(em.flags & 0x20u) != 0") != std::string::npos);
    assert(m2Particles.find("spinAngle = em.spin * p.life") != std::string::npos);
    assert(m2ParticleVert.find("layout(location = 4) in float aSpin") != std::string::npos);
    assert(m2ParticleVert.find("float cs = cos(aSpin)") != std::string::npos);
    assert(m2Renderer.find("pBind.stride = 21 * sizeof(float)") != std::string::npos);
    assert(m2Renderer.find("MAX_M2_RENDER_PARTICLES * 21 * sizeof(float)") != std::string::npos);

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
    assert(m2LoaderHeader.find("headCellBegin[2]") != std::string::npos);
    assert(m2LoaderHeader.find("headCellRepeat[2]") != std::string::npos);
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

    // Vanilla ribbon continuous tracks follow the current/cross-faded sequence,
    // while visibility remains a direct discrete/global-sequence sample. An
    // invalid/0xFFFF bone stays in model space rather than snapping to bone 0.
    assert(m2Particles.find("m2_track::sampleFloat(") != std::string::npos);
    assert(m2Particles.find("sampleM2BlendedFloat(gpu, inst, em.heightAboveTrack") != std::string::npos);
    assert(m2Particles.find("sampleM2BlendedVec3(") != std::string::npos);
    assert(m2Particles.find("gpu, inst, em.colorTrack") != std::string::npos);
    assert(m2Particles.find("boneIdx = 0") == std::string::npos);

    // Vanilla ribbon history keeps the emitter bone's cross-section axis,
    // interpolates newly emitted edges between consecutive poses, clamps the
    // reference lifetime/rate, and applies age-squared gravity sag.
    assert(m2Header.find("glm::vec3 upWorld") != std::string::npos);
    assert(m2Header.find("ribbonPrevSpines") != std::string::npos);
    assert(m2Header.find("ribbonPoseValid") != std::string::npos);
    assert(m2Particles.find("std::ceil(std::max(0.0f, em.edgesPerSecond))") != std::string::npos);
    assert(m2Particles.find("std::max(0.25f, em.edgeLifetime)") != std::string::npos);
    assert(m2Particles.find("inst.ribbonPrevSpines[ri]") != std::string::npos);
    assert(m2Particles.find("spineWorld, interpolation") != std::string::npos);
    assert(m2Particles.find("2.0f * em.gravity * ageBefore * simDt") != std::string::npos);
    assert(m2Particles.find("vanillaRendering_ ? e.upWorld") != std::string::npos);
    assert(m2Particles.find("e.age / normalizedLifetime") != std::string::npos);

    // Classic ribbon fog follows renderFlags.Unfogged and blend mode:
    // scene-colour fog for ordinary alpha, black fog for Add/AddAlpha.
    assert(m2LoaderHeader.find("uint16_t materialFlags = 0") != std::string::npos);
    assert(m2Particles.find("(em.materialFlags & 0x02u) != 0") != std::string::npos);
    assert(m2Particles.find("const uint8_t ribbonFogPolicy") != std::string::npos);
    assert(m2Particles.find("dst[written * 10 + 9] = static_cast<float>(ribbonFogPolicy)") != std::string::npos);
    assert(m2Renderer.find("rBind.stride = 10 * sizeof(float)") != std::string::npos);
    assert(m2Renderer.find("MAX_RIBBON_VERTS * 10 * sizeof(float)") != std::string::npos);
    assert(m2RibbonVert.find("layout(location = 4) in float aFogPolicy") != std::string::npos);
    assert(m2RibbonVert.find("flat out int vFogPolicy") != std::string::npos);
    assert(m2RibbonFrag.find("flat in int vFogPolicy") != std::string::npos);
    assert(m2RibbonFrag.find("vFogPolicy == 2 ? vec3(0.0) : fogColor.rgb") != std::string::npos);
    assert(m2RibbonFrag.find("rgb *= vFogFactor") == std::string::npos);

    // The ground-detail opaque gate must not reference a pass-local variable
    // before it is declared; this is also the authored Vanilla cutout exception.
    assert(m2Render.find("!(vanillaRendering_ && model.isGroundDetail)") != std::string::npos);

    // World-doodad fade keeps the 224/255 AlphaKey silhouette stable: object
    // fade affects source alpha/output blend, not the texel-alpha comparison.
    assert(m2.find("float alphaForTest = texColor.a") != std::string::npos);
    assert(m2.find("alphaForTest *= vFadeAlpha") == std::string::npos);
    assert(m2.find("float outAlpha = texColor.a * vFadeAlpha") != std::string::npos);
    // The Vanilla preset must not silently inherit name-based replacement
    // effects from the enhanced renderer, in either opaque or blended passes.
    assert(m2Particles.find("inst.forcedHidden || gpu.isInstancePortal") == std::string::npos);
    assert(m2Particles.find("if (gpu.isInstancePortal)") == std::string::npos);
    assert(m2Particles.find("!vanillaRendering_ && gpu.isInstancePortal") != std::string::npos);
    const auto smokeRender = m2Particles.substr(m2Particles.find("void M2Renderer::renderSmokeParticles("));
    assert(smokeRender.find("if (vanillaRendering_) return;") < smokeRender.find("vkCmdDraw"));
    assert(m2Render.find("if (!vanillaRendering_ && smokeEmitAccum >= emitInterval") != std::string::npos);
    assert(m2Render.find("if (!vanillaRendering_ && inst.cachedIsSmoke) flags |= 2u;") != std::string::npos);
    assert(m2Render.find("if (vanillaRendering_) break; // Preserve the placement") != std::string::npos);
    assert(m2Render.find("if (model.isLavaModel &&") == std::string::npos);
    assert(m2Render.find("|| model.isLavaModel;") == std::string::npos);
    assert(m2Render.find("if (!vanillaRendering_ && !skyMode_ && m2BlendIsAdditive") != std::string::npos);
    assert(m2Render.find("mat->tintR = batch.tint.r;") != std::string::npos);
    assert(m2.find("if (!vanillaRendering && colorKeyBlack != 0 && alphaTest == 0)") != std::string::npos);
    return 0;
}
