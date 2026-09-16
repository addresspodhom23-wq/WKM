#include "rendering/m2_renderer.hpp"
#include <unordered_set>
#include "rendering/m2_renderer_internal.h"
#include "rendering/vk_context.hpp"
#include "rendering/vk_buffer.hpp"
#include "rendering/vk_texture.hpp"
#include "rendering/vk_pipeline.hpp"
#include "rendering/vk_shader.hpp"
#include "rendering/vk_utils.hpp"
#include "rendering/camera.hpp"
#include "pipeline/asset_manager.hpp"
#include "core/logger.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <array>
#include <cmath>
#include <random>

namespace wowee {
namespace rendering {

namespace {

const std::array<float, 128>& vanillaTwinkleLut() {
    static const std::array<float, 128> lut = [] {
        std::array<float, 128> out{};
        uint32_t s = 0xC0FFEE11u;
        for (float& value : out) {
            s ^= s << 13;
            s ^= s >> 17;
            s ^= s << 5;
            value = static_cast<float>(s & 0x00FFFFFFu) / 16777215.0f;
        }
        return out;
    }();
    return lut;
}

float vanillaTwinkleNoise(float speed, float age, uint32_t phase) {
    const float w = speed * age;
    const uint32_t index = std::isfinite(w)
        ? static_cast<uint32_t>(glm::clamp(w, 0.0f, 255.0f))
        : 0u;
    return vanillaTwinkleLut()[(index + phase) & 0x7Fu];
}

} // namespace

// --- M2 Particle Emitter Helpers ---

float M2Renderer::interpFloat(const pipeline::M2AnimationTrack& track, float animTime,
                                float globalTime, int seqIdx,
                                const std::vector<uint32_t>& globalSeqDurations) {
    return m2_track::sampleFloat(track, seqIdx, animTime, globalTime,
                                 globalSeqDurations, 0.0f);
}

// Interpolate an M2 FBlock (particle lifetime curve) at a given life ratio [0..1].
// FBlocks store per-lifetime keyframes for particle color, alpha, and scale.
// NOTE: interpFBlockFloat and interpFBlockVec3 share identical interpolation logic -
// if you fix a bug in one, update the other to match.
float M2Renderer::interpFBlockFloat(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.floatValues.empty()) return 1.0f;
    if (fb.floatValues.size() == 1 || fb.timestamps.empty()) return fb.floatValues[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.floatValues.size() - 1);
            size_t v1 = std::min(i + 1, fb.floatValues.size() - 1);
            return glm::mix(fb.floatValues[v0], fb.floatValues[v1], frac);
        }
    }
    return fb.floatValues.back();
}

glm::vec3 M2Renderer::interpFBlockVec3(const pipeline::M2FBlock& fb, float lifeRatio) {
    if (fb.vec3Values.empty()) return glm::vec3(1.0f);
    if (fb.vec3Values.size() == 1 || fb.timestamps.empty()) return fb.vec3Values[0];
    lifeRatio = glm::clamp(lifeRatio, 0.0f, 1.0f);
    for (size_t i = 0; i < fb.timestamps.size() - 1; i++) {
        if (lifeRatio <= fb.timestamps[i + 1]) {
            float t0 = fb.timestamps[i];
            float t1 = fb.timestamps[i + 1];
            float dur = t1 - t0;
            float frac = (dur > 0.0f) ? (lifeRatio - t0) / dur : 0.0f;
            size_t v0 = std::min(i, fb.vec3Values.size() - 1);
            size_t v1 = std::min(i + 1, fb.vec3Values.size() - 1);
            return glm::mix(fb.vec3Values[v0], fb.vec3Values[v1], frac);
        }
    }
    return fb.vec3Values.back();
}

std::vector<glm::vec3> M2Renderer::getWaterVegetationPositions(const glm::vec3& camPos, float maxDist) const {
    std::vector<glm::vec3> result;
    float maxDistSq = maxDist * maxDist;
    for (const auto& inst : instances) {
        if (inst.forcedHidden) continue;
        if (!inst.cachedModel || !inst.cachedModel->isWaterVegetation) continue;
        glm::vec3 diff = inst.position - camPos;
        if (glm::dot(diff, diff) <= maxDistSq) {
            result.push_back(inst.position);
        }
    }
    return result;
}

void M2Renderer::emitParticles(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (inst.forcedHidden || gpu.isInstancePortal) return;

    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }

    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);
    std::uniform_int_distribution<int> distTile;

    for (size_t ei = 0; ei < gpu.particleEmitters.size(); ei++) {
        const auto& em = gpu.particleEmitters[ei];
        if (!em.enabled) continue;

        const float emitterVisible = m2_track::sampleFloat(
            em.visibilityTrack, inst.currentSequenceIndex, inst.animTime,
            inst.globalSequenceTime, gpu.globalSequenceDurations, 1.0f);
        if (emitterVisible <= 0.0f) {
            // Do not bank hidden-time emission and release it as a burst when
            // the authored visibility track turns the emitter back on.
            inst.emitterAccumulators[ei] = 0.0f;
            continue;
        }

        float rate = interpFloat(em.emissionRate, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        float life = interpFloat(em.lifespan, inst.animTime, inst.globalSequenceTime,
                                 inst.currentSequenceIndex, gpu.globalSequenceDurations);
        // What the player asked to see of it, before the floor below. The order
        // is the whole point: thinning first and flooring second lets a low
        // setting take smoke, dust and spell effects down while a candle is
        // pulled back up to the handful of particles that still reads as fire.
        if (!vanillaRendering_) rate *= particleDensity_;

        // A flame reads as a flame only when enough particles are alive at once.
        // Authored rates vary wildly for the same visual intent - a candle asks
        // for 40/s over half a second, CHANDELIER01 for 1/s over six seconds,
        // which sustains a single speck per candle and looks like a bare glow.
        // Steady-state population is rate x lifespan, so floor the rate against
        // the lifespan to hold every fixture at a comparable density. Do not lower
        // this: at 7 the flames disappear entirely, as they do under any change
        // that reduces how many particles are alive or how far they travel. The
        // effect is only visible because a scattering of particles reaches open
        // air, so thinning it drops the whole thing below the threshold.
        if (!vanillaRendering_ && rate > 0.0f && life > 0.0f &&
            (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire || gpu.isKoboldFlame)) {
            constexpr float kMinLiveParticles = 15.0f;
            rate = std::max(rate, kMinLiveParticles / std::max(life, 0.1f));
        }

        if (rate <= 0.0f || life <= 0.0f) {
            // Diagnostic: a lamp or flame whose emitter never fires produces a
            // fixture that glows but shows no flame. Report each model once so a
            // default-level log says whether emission is the cause.
            if (gpu.isLanternLike || gpu.isTorch || gpu.isBrazierOrFire) {
                static std::unordered_set<std::string> reported;
                if (reported.insert(gpu.name).second) {
                    LOG_WARNING("Flame emitter idle: '", gpu.name, "' emitter=", ei,
                                " rate=", rate, " life=", life,
                                " animTime=", inst.animTime,
                                " seqIdx=", inst.currentSequenceIndex,
                                " gsTime=", inst.globalSequenceTime,
                                " rateSeqs=", em.emissionRate.sequences.size(),
                                " lifeSeqs=", em.lifespan.sequences.size(),
                                " rateGlobalSeq=", em.emissionRate.globalSequence);
                }
            }
            continue;
        }

        inst.emitterAccumulators[ei] += rate * dt;

        while (inst.emitterAccumulators[ei] >= 1.0f && inst.particles.size() < MAX_M2_PARTICLES) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            p.life = 0.0f;
            p.maxLife = life;
            p.tileIndex = 0.0f;
            p.phase = particleRng_() & 0x7Fu;

            glm::mat4 boneXform = glm::mat4(1.0f);
            if (em.bone < inst.boneMatrices.size()) {
                boneXform = inst.boneMatrices[em.bone];
            }

            // M2 stores launch speed plus a fractional variation. Plane emitters
            // spawn across their authored rectangle. Sphere emitters spawn on a
            // shell between areaLength/areaWidth and normally travel radially.
            float speed = interpFloat(em.emissionSpeed, inst.animTime, inst.globalSequenceTime,
                                      inst.currentSequenceIndex, gpu.globalSequenceDurations);
            const float speedVariation = interpFloat(
                em.speedVariation, inst.animTime, inst.globalSequenceTime,
                inst.currentSequenceIndex, gpu.globalSequenceDurations);
            const float vRange = interpFloat(
                em.verticalRange, inst.animTime, inst.globalSequenceTime,
                inst.currentSequenceIndex, gpu.globalSequenceDurations);
            const float hRange = interpFloat(
                em.horizontalRange, inst.animTime, inst.globalSequenceTime,
                inst.currentSequenceIndex, gpu.globalSequenceDurations);
            const float areaLength = interpFloat(
                em.emissionAreaLength, inst.animTime, inst.globalSequenceTime,
                inst.currentSequenceIndex, gpu.globalSequenceDurations);
            const float areaWidth = interpFloat(
                em.emissionAreaWidth, inst.animTime, inst.globalSequenceTime,
                inst.currentSequenceIndex, gpu.globalSequenceDurations);
            const float zSource = interpFloat(
                em.zSource, inst.animTime, inst.globalSequenceTime,
                inst.currentSequenceIndex, gpu.globalSequenceDurations);

            glm::vec3 localPos = em.position;
            glm::vec3 emissionOffset(0.0f);
            glm::vec3 dir(0.0f, 0.0f, 1.0f);

            if (vanillaRendering_) {
                if (em.emitterType == 2) {
                    // Sphere: areaLength/areaWidth are min/max shell radius.
                    const float inner = std::min(areaLength, areaWidth);
                    const float outer = std::max(areaLength, areaWidth);
                    const float radius = inner + dist01(particleRng_) *
                                         std::max(0.0f, outer - inner);
                    const float latitude = distN(particleRng_) * vRange;
                    const float longitude = distN(particleRng_) * hRange;
                    const float clat = std::cos(latitude);
                    const glm::vec3 shell(
                        clat * std::cos(longitude),
                        clat * std::sin(longitude),
                        std::sin(latitude));

                    emissionOffset = shell * radius;
                    localPos += emissionOffset;
                    if (zSource != 0.0f) {
                        dir = emissionOffset - glm::vec3(0.0f, 0.0f, zSource);
                        const float d2 = glm::dot(dir, dir);
                        dir = d2 > 1e-12f ? dir * glm::inversesqrt(d2)
                                         : glm::vec3(0.0f, 0.0f, 1.0f);
                    } else {
                        // 0x100 is the authored sphere-up behaviour. Otherwise
                        // velocity follows the radial shell direction; negative
                        // authored speed therefore produces a converging emitter.
                        dir = (em.flags & 0x100u)
                            ? glm::vec3(0.0f, 0.0f, 1.0f) : shell;
                    }
                } else {
                    // Plane (and rare spline fallback): uniform rectangle in
                    // emitter-local XY, then a symmetric cone around local +Z.
                    emissionOffset = glm::vec3(
                        areaLength * 0.5f * distN(particleRng_),
                        areaWidth  * 0.5f * distN(particleRng_),
                        0.0f);
                    localPos += emissionOffset;
                    if (zSource != 0.0f) {
                        dir = emissionOffset - glm::vec3(0.0f, 0.0f, zSource);
                        const float d2 = glm::dot(dir, dir);
                        dir = d2 > 1e-12f ? dir * glm::inversesqrt(d2)
                                         : glm::vec3(0.0f, 0.0f, 1.0f);
                    } else {
                        const float polar = distN(particleRng_) * vRange;
                        const float azimuth = distN(particleRng_) * hRange;
                        const float sinPolar = std::sin(polar);
                        dir = glm::vec3(
                            sinPolar * std::cos(azimuth),
                            sinPolar * std::sin(azimuth),
                            std::cos(polar));
                    }
                }

                // The 1.12 emitter kernel prepends R(+Z,90°) before the
                // bone/model transform. This is observable on non-square plane
                // emitters and on rotating model-space effects.
                auto rot90Z = [](const glm::vec3& v) {
                    return glm::vec3(-v.y, v.x, v.z);
                };
                emissionOffset = rot90Z(emissionOffset);
                dir = rot90Z(dir);
                localPos = em.position + emissionOffset;

                // Variation is a fraction of the authored speed. Do not clamp:
                // negative speed is intentional for inward-moving sphere FX.
                speed *= 1.0f + speedVariation * distN(particleRng_);
            } else {
                // Preserve Kraken's historical non-Vanilla spread behaviour.
                dir.x += distN(particleRng_) * hRange;
                dir.y += distN(particleRng_) * hRange;
                dir.z += distN(particleRng_) * vRange;
                const float lenSq = glm::dot(dir, dir);
                if (lenSq > 0.001f * 0.001f) dir *= glm::inversesqrt(lenSq);
            }

            const bool modelSpace =
                vanillaRendering_ && ((em.flags & 0x10u) != 0);
            glm::mat3 rotMat = glm::mat3(inst.modelMatrix * boneXform);
            if (modelSpace) {
                // Keep position/velocity in emitter-bone local space. The live
                // bone matrix is applied again at draw time, which is what makes
                // portal/swirl particles ride the rotating frame for their
                // entire lifetime instead of freezing the birth transform.
                p.position = localPos;
                p.velocity = dir * speed;
            } else {
                p.position = glm::vec3(
                    inst.modelMatrix * boneXform * glm::vec4(localPos, 1.0f));
                p.velocity = rotMat * dir * speed;
            }

            // Kraken-only fallback for models whose authored/external animation
            // data is incomplete. Vanilla mode must keep zero speed as zero.
            if (!vanillaRendering_ && std::abs(speed) < 0.01f) {
                if (gpu.isFireflyEffect) {
                    // Fireflies: gentle random drift in all directions
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.6f,
                        distN(particleRng_) * 0.3f
                    );
                } else {
                    p.velocity = rotMat * glm::vec3(
                        distN(particleRng_) * 1.0f,
                        distN(particleRng_) * 1.0f,
                        -dist01(particleRng_) * 0.5f
                    );
                }
            }

            const uint32_t tilesX = std::max<uint16_t>(em.textureCols, 1);
            const uint32_t tilesY = std::max<uint16_t>(em.textureRows, 1);
            const uint32_t totalTiles = tilesX * tilesY;
            if (!vanillaRendering_ && (em.flags & kParticleFlagTiled) && totalTiles > 1) {
                if (em.flags & kParticleFlagRandomized) {
                    distTile = std::uniform_int_distribution<int>(0, static_cast<int>(totalTiles - 1));
                    p.tileIndex = static_cast<float>(distTile(particleRng_));
                } else {
                    p.tileIndex = 0.0f;
                }
            }

            inst.particles.push_back(p);

            // Diagnostic: log first particle birth per spell effect instance
            if (gpu.isSpellEffect && inst.particles.size() == 1) {
                LOG_INFO("SpellEffect: first particle for '", gpu.name,
                         "' pos=(", p.position.x, ",", p.position.y, ",", p.position.z,
                         ") rate=", rate, " life=", life,
                         " bone=", em.bone, " boneCount=", inst.boneMatrices.size(),
                         " globalSeqs=", gpu.globalSequenceDurations.size());
            }
        }
        // Cap accumulator to avoid bursts after lag
        if (inst.emitterAccumulators[ei] > 2.0f) {
            inst.emitterAccumulators[ei] = 0.0f;
        }
    }
}

void M2Renderer::updateParticles(M2Instance& inst, float dt) {
    if (!inst.cachedModel) return;
    const auto& gpu = *inst.cachedModel;

    // Hoist per-emitter gravity out of the per-particle loop. Gravity (and the
    // emissionSpeed fallback) depends only on the emitter and animation time -
    // not on the particle itself - so interpFloat was being re-evaluated for
    // every particle even when 100s of particles share one emitter.
    constexpr size_t kMaxStackEmitters = 16;
    float emitterGravStack[kMaxStackEmitters];
    std::vector<float> emitterGravHeap;
    const size_t numEm = gpu.particleEmitters.size();
    float* emitterGrav = nullptr;
    if (numEm > 0) {
        if (numEm <= kMaxStackEmitters) {
            emitterGrav = emitterGravStack;
        } else {
            emitterGravHeap.resize(numEm);
            emitterGrav = emitterGravHeap.data();
        }
        for (size_t e = 0; e < numEm; ++e) {
            const auto& pem = gpu.particleEmitters[e];
            float grav = interpFloat(pem.gravity,
                                      inst.animTime, inst.globalSequenceTime,
                                      inst.currentSequenceIndex, gpu.globalSequenceDurations);
            if (!vanillaRendering_ && grav == 0.0f && !gpu.isFireflyEffect) {
                float emSpeed = interpFloat(pem.emissionSpeed,
                                             inst.animTime, inst.globalSequenceTime,
                                             inst.currentSequenceIndex, gpu.globalSequenceDurations);
                grav = (std::abs(emSpeed) > 0.1f) ? 4.0f : 1.5f;
            }
            emitterGrav[e] = grav;
        }
    }

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        p.life += dt;
        if (p.life >= p.maxLife) {
            // Swap-and-pop removal
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }
        if (p.emitterIndex >= 0 && static_cast<size_t>(p.emitterIndex) < numEm) {
            const auto& em = gpu.particleEmitters[static_cast<size_t>(p.emitterIndex)];
            const float grav = emitterGrav[p.emitterIndex];
            const bool modelSpace =
                vanillaRendering_ && ((em.flags & 0x10u) != 0);

            if (modelSpace) {
                // Reference simulation clamps long frames, advances on the
                // pre-gravity velocity, then applies the closed-form half-step.
                const float sdt = std::min(std::max(dt, 0.0f), 0.1f);
                p.position += p.velocity * sdt;
                if (grav != 0.0f) {
                    p.position.z -= 0.5f * grav * sdt * sdt;
                    p.velocity.z -= grav * sdt;
                }
                if (em.drag > 0.0f) {
                    const float drag = std::min(sdt * em.drag, 1.0f);
                    p.velocity -= drag * p.velocity;
                }
            } else {
                p.velocity.z -= grav * dt;
                if (vanillaRendering_ && em.drag > 0.0f) {
                    const float drag = std::min(std::max(dt, 0.0f) * em.drag, 1.0f);
                    p.velocity -= drag * p.velocity;
                }
                p.position += p.velocity * dt;
            }
        } else {
            p.position += p.velocity * dt;
        }
        i++;
    }
}

// ---------------------------------------------------------------------------
// Ribbon emitter simulation
// ---------------------------------------------------------------------------
void M2Renderer::updateRibbons(M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (gpu.isInstancePortal) return;

    const auto& emitters = gpu.ribbonEmitters;
    if (emitters.empty()) return;

    const float rawDt = std::isfinite(dt) ? std::max(0.0f, dt) : 0.0f;
    const float simDt = std::min(rawDt, 0.1f);

    // Grow per-instance state arrays if needed.
    if (inst.ribbonEdges.size() != emitters.size())
        inst.ribbonEdges.resize(emitters.size());
    if (inst.ribbonEdgeAccumulators.size() != emitters.size())
        inst.ribbonEdgeAccumulators.resize(emitters.size(), 0.0f);
    if (inst.ribbonPrevSpines.size() != emitters.size())
        inst.ribbonPrevSpines.resize(emitters.size(), glm::vec3(0.0f));
    if (inst.ribbonPrevUps.size() != emitters.size())
        inst.ribbonPrevUps.resize(emitters.size(), glm::vec3(0.0f, 0.0f, 1.0f));
    if (inst.ribbonPoseValid.size() != emitters.size())
        inst.ribbonPoseValid.resize(emitters.size(), 0);

    for (size_t ri = 0; ri < emitters.size(); ri++) {
        const auto& em = emitters[ri];
        auto& edges = inst.ribbonEdges[ri];
        auto& accum = inst.ribbonEdgeAccumulators[ri];

        // The ribbon cross section follows the emitter bone's authored local +Z
        // axis. Using world +Z made weapon trails twist or flatten when the hand
        // rotated, even though their centerline followed the correct bone.
        glm::mat4 emitterWorld = inst.modelMatrix;
        if (em.bone < inst.boneMatrices.size())
            emitterWorld = inst.modelMatrix * inst.boneMatrices[em.bone];

        const glm::vec3 spineWorld = glm::vec3(
            emitterWorld * glm::vec4(em.position, 1.0f));
        glm::vec3 upWorld = glm::mat3(emitterWorld) * glm::vec3(0.0f, 0.0f, 1.0f);
        const float upLen2 = glm::dot(upWorld, upWorld);
        if (upLen2 > 1e-12f && std::isfinite(upLen2))
            upWorld *= glm::inversesqrt(upLen2);
        else
            upWorld = glm::vec3(0.0f, 0.0f, 1.0f);

        if (!std::isfinite(spineWorld.x) || !std::isfinite(spineWorld.y) ||
            !std::isfinite(spineWorld.z)) {
            continue;
        }

        const auto& gsd = gpu.globalSequenceDurations;
        const float visibility = m2_track::sampleFloat(
            em.visibilityTrack, inst.currentSequenceIndex, inst.animTime,
            inst.globalSequenceTime, gsd, 1.0f);
        const float heightAbove = std::max(0.0f, m2_track::sampleFloat(
            em.heightAboveTrack, inst.currentSequenceIndex, inst.animTime,
            inst.globalSequenceTime, gsd, 0.0f));
        const float heightBelow = std::max(0.0f, m2_track::sampleFloat(
            em.heightBelowTrack, inst.currentSequenceIndex, inst.animTime,
            inst.globalSequenceTime, gsd, 0.0f));
        const glm::vec3 color = m2_track::sampleVec3(
            em.colorTrack, inst.currentSequenceIndex, inst.animTime,
            inst.globalSequenceTime, gsd, glm::vec3(1.0f));
        const float alpha = glm::clamp(m2_track::sampleFloat(
            em.alphaTrack, inst.currentSequenceIndex, inst.animTime,
            inst.globalSequenceTime, gsd, 1.0f), 0.0f, 1.0f);

        // Vanilla normalizes these two scalar controls before simulation.
        const float edgeRate = std::isfinite(em.edgesPerSecond)
            ? std::ceil(std::max(0.0f, em.edgesPerSecond)) : 0.0f;
        const float edgeLifetime = std::isfinite(em.edgeLifetime)
            ? std::max(0.25f, em.edgeLifetime) : 0.25f;

        // Age old committed edges. Gravity is a function of the edge's age:
        // displacement is g*t^2, so one step adds
        // g*((t+dt)^2 - t^2) = 2*g*t*dt + g*dt^2.
        for (auto& edge : edges) {
            const float ageBefore = edge.age;
            edge.age += rawDt;
            if (em.gravity != 0.0f && simDt > 0.0f) {
                const float sag =
                    2.0f * em.gravity * ageBefore * simDt +
                    em.gravity * simDt * simDt;
                edge.worldPos.z -= sag;
            }
        }
        while (!edges.empty() && edges.front().age > edgeLifetime)
            edges.pop_front();

        const bool firstPose = inst.ribbonPoseValid[ri] == 0;
        if (firstPose) {
            inst.ribbonPrevSpines[ri] = spineWorld;
            inst.ribbonPrevUps[ri] = upWorld;
            inst.ribbonPoseValid[ri] = 1;
        }

        if (visibility > 0.5f && edgeRate > 0.0f) {
            const float previousPhase = accum;
            float edgeProgress = previousPhase + edgeRate * simDt;
            // The source and ribbon share a birth edge. Ensure the first live
            // update commits it even if the first frame is shorter than 1/rate.
            if (firstPose && edges.empty())
                edgeProgress = std::max(edgeProgress, 1.0001f);

            const uint32_t edgeCount =
                static_cast<uint32_t>(std::floor(edgeProgress));
            const float phaseDelta = edgeProgress - previousPhase;

            const size_t capacity = std::clamp<size_t>(
                static_cast<size_t>(std::ceil(edgeLifetime * edgeRate)) + 2u,
                2u, 512u);

            for (uint32_t edgeIndex = 0; edgeIndex < edgeCount; ++edgeIndex) {
                const float targetPhase = static_cast<float>(edgeIndex + 1u);
                const float interpolation = phaseDelta > 0.0f
                    ? glm::clamp((targetPhase - previousPhase) / phaseDelta,
                                 0.0f, 1.0f)
                    : 1.0f;

                M2Instance::RibbonEdge edge{};
                edge.worldPos = glm::mix(
                    inst.ribbonPrevSpines[ri], spineWorld, interpolation);
                edge.upWorld = glm::mix(
                    inst.ribbonPrevUps[ri], upWorld, interpolation);
                const float axisLen2 = glm::dot(edge.upWorld, edge.upWorld);
                if (axisLen2 > 1e-12f && std::isfinite(axisLen2))
                    edge.upWorld *= glm::inversesqrt(axisLen2);
                else
                    edge.upWorld = upWorld;
                edge.color = color;
                edge.alpha = alpha;
                edge.heightAbove = heightAbove;
                edge.heightBelow = heightBelow;
                edge.age = 0.0f;
                edges.push_back(edge);

                while (edges.size() > capacity)
                    edges.pop_front();

                if (gpu.isSpellEffect && edges.size() == 1) {
                    LOG_INFO("SpellEffect: ribbon edge[0] for '", gpu.name,
                             "' emitter=", ri, " pos=(", edge.worldPos.x, ",",
                             edge.worldPos.y, ",", edge.worldPos.z, ") hA=",
                             heightAbove, " hB=", heightBelow,
                             " vis=", visibility, " eps=", edgeRate,
                             " edgeLife=", edgeLifetime, " bone=", em.bone);
                }
            }

            accum = edgeProgress - std::floor(edgeProgress);
        } else {
            accum = 0.0f;
        }

        inst.ribbonPrevSpines[ri] = spineWorld;
        inst.ribbonPrevUps[ri] = upWorld;
    }
}

// ---------------------------------------------------------------------------
// Ribbon rendering
// ---------------------------------------------------------------------------
void M2Renderer::renderM2Ribbons(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!ribbonPipeline_ || !ribbonVB_ || !ribbonVBMapped_) return;
    // Diagnostic: WOWEE_M2_NO_RIBBONS=1 drops every M2 ribbon trail draw.
    static const bool kNoRibbons = envFlagEnabled("WOWEE_M2_NO_RIBBONS");
    if (kNoRibbons) return;

    float* dst     = static_cast<float*>(ribbonVBMapped_);
    size_t written = 0;

    ribbonDraws_.clear();
    auto& draws = ribbonDraws_;

    for (const auto& inst : instances) {
        if (inst.forcedHidden) continue;
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.isInstancePortal) continue;
        if (gpu.ribbonEmitters.empty()) continue;

        for (size_t ri = 0; ri < gpu.ribbonEmitters.size(); ri++) {
            if (ri >= inst.ribbonEdges.size()) continue;
            const auto& edges = inst.ribbonEdges[ri];
            if (edges.size() < 2) continue;

            const auto& em = gpu.ribbonEmitters[ri];

            // Ribbon materialIndices[] point into the M2 material table; route
            // the authored Vanilla blend mode exactly instead of collapsing
            // Add/AddAlpha/Mod/Mod2x into one additive state.
            VkPipeline pipe = ribbonPipeline_;
            switch (em.blendMode) {
                case 0:
                case 1: pipe = ribbonOpaquePipeline_; break;
                case 2: pipe = ribbonPipeline_; break;
                case 3:
                case 4: pipe = ribbonAdditivePipeline_; break;
                case 5: pipe = ribbonModulatePipeline_; break;
                case 6: pipe = ribbonModulate2xPipeline_; break;
                default: break;
            }

            // textureSlotTrack selects a slot in the ribbon's direct
            // textureIndices[] array. This is animation/global-sequence aware.
            size_t textureSlot = 0;
            if (!em.textureIndices.empty()) {
                const float slotValue = m2_track::sampleFloat(
                    em.textureSlotTrack, inst.currentSequenceIndex, inst.animTime,
                    inst.globalSequenceTime, gpu.globalSequenceDurations, 0.0f);
                const long roundedSlot = std::lround(slotValue);
                if (roundedSlot > 0) {
                    textureSlot = std::min<size_t>(
                        static_cast<size_t>(roundedSlot),
                        em.textureIndices.size() - 1);
                }
            }

            VkDescriptorSet texSet = VK_NULL_HANDLE;
            if (ri < gpu.ribbonTexSets.size() &&
                textureSlot < gpu.ribbonTexSets[ri].size()) {
                texSet = gpu.ribbonTexSets[ri][textureSlot];
            }
            if (!texSet) {
                if (gpu.isSpellEffect) {
                    static bool ribbonTexWarn = false;
                    if (!ribbonTexWarn) {
                        LOG_WARNING("SpellEffect: ribbon[", ri, "] for '", gpu.name,
                                    "' has null texSet - descriptor pool may be exhausted");
                        ribbonTexWarn = true;
                    }
                }
                continue;
            }

            const float normalizedLifetime = std::isfinite(em.edgeLifetime)
                ? std::max(0.25f, em.edgeLifetime) : 0.25f;
            glm::vec3 liveColor(1.0f);
            float liveAlpha = 1.0f;
            if (vanillaRendering_) {
                liveColor = m2_track::sampleVec3(
                    em.colorTrack, inst.currentSequenceIndex, inst.animTime,
                    inst.globalSequenceTime, gpu.globalSequenceDurations,
                    glm::vec3(1.0f));
                liveAlpha = glm::clamp(m2_track::sampleFloat(
                    em.alphaTrack, inst.currentSequenceIndex, inst.animTime,
                    inst.globalSequenceTime, gpu.globalSequenceDurations, 1.0f),
                    0.0f, 1.0f);
            }

            uint32_t firstVert = static_cast<uint32_t>(written);

            // Emit triangle strip: 2 verts per edge (top + bottom).
            for (size_t ei = 0; ei < edges.size(); ei++) {
                if (written + 2 > MAX_RIBBON_VERTS) break;
                const auto& e = edges[ei];
                const glm::vec3 edgeAxis =
                    vanillaRendering_ ? e.upWorld : glm::vec3(0.0f, 0.0f, 1.0f);
                const glm::vec3 edgeColor =
                    vanillaRendering_ ? liveColor : e.color;
                const float a = vanillaRendering_
                    ? liveAlpha
                    : e.alpha * ((em.edgeLifetime > 0.0f)
                        ? std::max(0.0f, 1.0f - e.age / em.edgeLifetime) : 1.0f);
                // Vanilla maps ribbon U from edge age/lifetime rather than by
                // current deque index, so dropped/late frames do not stretch the
                // entire texture across a shorter strip.
                const float u = vanillaRendering_
                    ? glm::clamp(e.age / normalizedLifetime, 0.0f, 1.0f)
                    : static_cast<float>(ei) /
                      static_cast<float>(edges.size() - 1);

                glm::vec3 top = e.worldPos + edgeAxis * e.heightAbove;
                dst[written * 9 + 0] = top.x;
                dst[written * 9 + 1] = top.y;
                dst[written * 9 + 2] = top.z;
                dst[written * 9 + 3] = edgeColor.r;
                dst[written * 9 + 4] = edgeColor.g;
                dst[written * 9 + 5] = edgeColor.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 0.0f;
                written++;

                glm::vec3 bot = e.worldPos - edgeAxis * e.heightBelow;
                dst[written * 9 + 0] = bot.x;
                dst[written * 9 + 1] = bot.y;
                dst[written * 9 + 2] = bot.z;
                dst[written * 9 + 3] = edgeColor.r;
                dst[written * 9 + 4] = edgeColor.g;
                dst[written * 9 + 5] = edgeColor.b;
                dst[written * 9 + 6] = a;
                dst[written * 9 + 7] = u;
                dst[written * 9 + 8] = 1.0f;
                written++;
            }

            uint32_t vertCount = static_cast<uint32_t>(written) - firstVert;
            if (vertCount >= 4) {
                draws.push_back({.texSet = texSet, .pipeline = pipe, .firstVertex = firstVert, .vertexCount = vertCount});
            } else {
                // Rollback if too few verts
                written = firstVert;
            }
        }
    }

    // Periodic diagnostic: spell ribbon draw count
    {
        static uint32_t ribbonDiagFrame_ = 0;
        if (++ribbonDiagFrame_ % 300 == 1) {
            size_t spellRibbonDraws = 0;
            size_t spellRibbonVerts = 0;
            for (const auto& inst : instances) {
                if (!inst.cachedModel || !inst.cachedModel->isSpellEffect) continue;
                for (const auto& ribbonEdge : inst.ribbonEdges) {
                    if (ribbonEdge.size() >= 2) {
                        spellRibbonDraws++;
                        spellRibbonVerts += ribbonEdge.size() * 2;
                    }
                }
            }
            if (spellRibbonDraws > 0 || !draws.empty()) {
                LOG_INFO("SpellEffect: ", spellRibbonDraws, " spell ribbon strips (",
                         spellRibbonVerts, " verts), total draws=", draws.size(),
                         " written=", written);
            }
        }
    }

    if (draws.empty() || written == 0) return;

    VkExtent2D ext = vkCtx_->getSwapchainExtent();
    VkViewport vp{};
    vp.x = 0; vp.y = 0;
    vp.width  = static_cast<float>(ext.width);
    vp.height = static_cast<float>(ext.height);
    vp.minDepth = 0.0f; vp.maxDepth = 1.0f;
    VkRect2D sc{};
    sc.offset = {.x = 0, .y = 0};
    sc.extent = ext;
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);

    VkPipeline lastPipe = VK_NULL_HANDLE;
    for (const auto& dc : draws) {
        if (dc.pipeline != lastPipe) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, dc.pipeline);
            vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    ribbonPipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);
            lastPipe = dc.pipeline;
        }
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                ribbonPipelineLayout_, 1, 1, &dc.texSet, 0, nullptr);
        VkDeviceSize offset = 0;
        vkCmdBindVertexBuffers(cmd, 0, 1, &ribbonVB_, &offset);
        vkCmdDraw(cmd, dc.vertexCount, 1, dc.firstVertex, 0);
    }
}

void M2Renderer::renderM2Particles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (!particlePipeline_ || !m2ParticleVB_) return;
    // Diagnostic: WOWEE_M2_NO_PARTICLES=1 drops every M2 particle draw, which
    // tells a particle artifact apart from a skinned-geometry one.
    static const bool kNoParticles = envFlagEnabled("WOWEE_M2_NO_PARTICLES");
    if (kNoParticles) return;

    // Collect all particles from all instances, grouped by texture+blend
    // Reuse persistent map - clear each group's vertex data but keep bucket structure.
    for (auto& [k, g] : particleGroups_) {
        g.vertexData.clear();
        g.preAllocSet = VK_NULL_HANDLE;
    }
    auto& groups = particleGroups_;

    size_t totalParticles = 0;

    for (auto& inst : instances) {
        if (inst.forcedHidden) continue;
        if (inst.particles.empty()) continue;
        if (!inst.cachedModel) continue;
        const auto& gpu = *inst.cachedModel;
        if (gpu.isInstancePortal) continue;


        // Cache the last emitter's per-emitter state so adjacent particles
        // sharing an emitter (the common case - particles from one source
        // cluster together) skip the texture/key/map-lookup work entirely.
        int lastEmitterIdx = -1;
        VkTexture* cachedTex = nullptr;
        uint16_t cachedTilesX = 1, cachedTilesY = 1;
        uint32_t cachedTotalTiles = 1;
        uint16_t cachedBlendType = 0;
        const pipeline::M2ParticleEmitter* cachedEm = nullptr;
        ParticleGroup* cachedGroup = nullptr;
        // animFrame depends only on inst.animTime + totalTiles, so it's also
        // emitter-stable within one frame.
        uint32_t cachedAnimFrame = 0;
        float cachedTilesFloat = 1.0f;
        bool cachedIsTiled = false;
        float invAnimMs = 1.0f / 1000.0f;

        for (const auto& p : inst.particles) {
            if (p.emitterIndex < 0 || p.emitterIndex >= static_cast<int>(gpu.particleEmitters.size())) continue;

            if (p.emitterIndex != lastEmitterIdx) {
                lastEmitterIdx = p.emitterIndex;
                cachedEm = &gpu.particleEmitters[p.emitterIndex];

                cachedTex = whiteTexture_.get();
                if (p.emitterIndex < static_cast<int>(gpu.particleTextures.size())) {
                    cachedTex = gpu.particleTextures[p.emitterIndex];
                }
                cachedTilesX = std::max<uint16_t>(cachedEm->textureCols, 1);
                cachedTilesY = std::max<uint16_t>(cachedEm->textureRows, 1);
                cachedTotalTiles = static_cast<uint32_t>(cachedTilesX) *
                                   static_cast<uint32_t>(cachedTilesY);
                cachedBlendType = cachedEm->blendingType;
                ParticleGroupKey key{.texture = cachedTex, .blendType = static_cast<uint8_t>(cachedBlendType), .tilesX = cachedTilesX, .tilesY = cachedTilesY};
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                if (cachedGroup->preAllocSet == VK_NULL_HANDLE &&
                    p.emitterIndex < static_cast<int>(gpu.particleTexSets.size())) {
                    cachedGroup->preAllocSet = gpu.particleTexSets[p.emitterIndex];
                }

                cachedIsTiled = (cachedEm->flags & kParticleFlagTiled) && cachedTotalTiles > 1;
                if (cachedIsTiled) {
                    float animSeconds = inst.animTime * invAnimMs;
                    cachedAnimFrame = static_cast<uint32_t>(std::floor(animSeconds * cachedTotalTiles))
                                      % cachedTotalTiles;
                    cachedTilesFloat = static_cast<float>(cachedTotalTiles);
                }
            }

            const auto& em = *cachedEm;
            float lifeRatio = p.life / std::max(p.maxLife, 0.001f);
            glm::vec3 color = interpFBlockVec3(em.particleColor, lifeRatio);
            float alpha = std::min(interpFBlockFloat(em.particleAlpha, lifeRatio), 1.0f);
            float rawScale = interpFBlockFloat(em.particleScale, lifeRatio);

            float twinkleNoise = 0.0f;
            if (vanillaRendering_) {
                twinkleNoise = vanillaTwinkleNoise(
                    em.twinkleSpeed, p.life, p.phase);
                if (em.twinklePercent < 1.0f &&
                    twinkleNoise > em.twinklePercent) {
                    continue;
                }
            }

            if (!vanillaRendering_ &&
                !gpu.isSpellEffect && !gpu.isFireflyEffect && !gpu.isLanternLike &&
                !gpu.isTorch && !gpu.isBrazierOrFire && !gpu.isKoboldFlame) {
                color = glm::mix(color, glm::vec3(1.0f), 0.7f);
                if (rawScale > 2.0f) alpha *= 0.02f;
                if (cachedBlendType == 3 || cachedBlendType == 4) alpha *= 0.05f;
            }
            // Flame fixtures: the authored curves can leave a particle with
            // effectively no colour or alpha for most of its life. CHANDELIER01
            // ramps scale from zero over a six second life and its candles spend
            // nearly all of that time contributing nothing - and because these
            // draw additively, a near-black particle adds literally nothing to
            // the frame. Floor colour and alpha so a lit fixture always shows
            // flame. Floors only lift the dim end, leaving torches and candles
            // that already read correctly untouched.
            if (!vanillaRendering_ &&
                (gpu.isLanternLike || gpu.isTorch ||
                 gpu.isBrazierOrFire || gpu.isKoboldFlame)) {
                color = glm::max(color, glm::vec3(0.50f, 0.26f, 0.09f));
                alpha = std::max(alpha, 0.30f);
            }

            float scale = rawScale;
            if (vanillaRendering_) {
                // Degenerate twinkle ranges (0/0 and 1/1 alike) mean steady
                // scale. Otherwise the deterministic LUT modulates authored size.
                if (std::abs(em.twinkleMax - em.twinkleMin) >= 1e-6f) {
                    scale *= glm::mix(
                        em.twinkleMin, em.twinkleMax, twinkleNoise);
                }
                // Vanilla applies placement scale to particle half-size only
                // when the emitter authors the 0x20 inherit-scale flag.
                if ((em.flags & 0x20u) != 0)
                    scale *= inst.scale;
            }
            if (!vanillaRendering_ && gpu.isSpellEffect) {
                scale = std::max(rawScale * 1.5f, 0.15f);
            } else if (!vanillaRendering_ && !gpu.isFireflyEffect) {
                scale = std::min(rawScale, 1.5f);
                // Candle flames are authored at a fraction of a unit, which lands
                // sub-pixel at any normal viewing distance - the fixture glows
                // with no visible flame. Small effect-heavy models dodge this by
                // being classified as spell effects (three or more emitters and
                // few vertices) and picking up that path's floor, but a
                // chandelier is a real fixture at 370 vertices and misses the
                // cut, so its five candles rendered as nothing. Give flame
                // fixtures the same floor without making them spell effects,
                // which would also change how they blend.
                if (gpu.isLanternLike || gpu.isTorch ||
                    gpu.isBrazierOrFire || gpu.isKoboldFlame) {
                    scale = std::max(scale, 0.15f);
                }
            }

            glm::vec3 drawPos = p.position;
            if (vanillaRendering_ && ((em.flags & 0x10u) != 0)) {
                glm::mat4 liveBone(1.0f);
                if (em.bone < inst.boneMatrices.size())
                    liveBone = inst.boneMatrices[em.bone];
                drawPos = glm::vec3(
                    inst.modelMatrix * liveBone * glm::vec4(p.position, 1.0f));
            }

            glm::vec3 drawVelocity = p.velocity;
            if (vanillaRendering_ && ((em.flags & 0x10u) != 0)) {
                glm::mat4 liveBone(1.0f);
                if (em.bone < inst.boneMatrices.size())
                    liveBone = inst.boneMatrices[em.bone];
                drawVelocity = glm::mat3(inst.modelMatrix * liveBone) * p.velocity;
            }

            const float tLife = glm::clamp(lifeRatio, 0.0f, 1.0f);
            const float mid = glm::clamp(em.lifeMidpoint, 0.001f, 1.0f);
            const int seg = tLife <= mid ? 0 : 1;
            float segT = seg == 0
                ? tLife / mid
                : (tLife - mid) / std::max(1.0f - mid, 0.001f);
            segT = glm::clamp(segT, 0.0f, 1.0f) * 0.99f + 0.005f;
            const float repeat = static_cast<float>(em.headCellRepeat[seg]);
            const float cellT = repeat != 1.0f
                ? segT * repeat - std::floor(segT * repeat)
                : segT;

            auto sampleCell = [&](const uint16_t begin[2],
                                  const uint16_t finish[2]) -> float {
                if (cachedTotalTiles <= 1) return 0.0f;
                const int b = static_cast<int>(begin[seg]);
                const int e = static_cast<int>(finish[seg]);
                const int baseCell = e >= b ? b : b + 1;
                const int span = e >= b ? e - b + 1 : e - b - 1;
                const int authoredCell =
                    static_cast<int>(std::floor(baseCell + span * cellT)) & 0xFF;
                return static_cast<float>(
                    static_cast<uint32_t>(authoredCell) % cachedTotalTiles);
            };

            float headTileIndex = p.tileIndex;
            if (vanillaRendering_ && cachedTotalTiles > 1) {
                headTileIndex = sampleCell(em.headCellBegin, em.headCellEnd);
            } else if (cachedIsTiled) {
                headTileIndex = p.tileIndex + static_cast<float>(cachedAnimFrame);
                while (headTileIndex >= cachedTilesFloat)
                    headTileIndex -= cachedTilesFloat;
            }

            float spinAngle = 0.0f;
            if (vanillaRendering_ && em.spin != 0.0f) {
                spinAngle = em.spin * p.life;
                if (spinAngle < 0.0f && (p.phase & 0x20u) != 0)
                    spinAngle = -spinAngle;
            }

            auto appendRecord = [&](float tileIndex, float spin,
                                    float tailSeconds, float tailMode) {
                auto& vd = cachedGroup->vertexData;
                vd.push_back(drawPos.x);
                vd.push_back(drawPos.y);
                vd.push_back(drawPos.z);
                vd.push_back(color.r);
                vd.push_back(color.g);
                vd.push_back(color.b);
                vd.push_back(alpha);
                vd.push_back(scale);
                vd.push_back(tileIndex);
                vd.push_back(spin);
                vd.push_back(drawVelocity.x);
                vd.push_back(drawVelocity.y);
                vd.push_back(drawVelocity.z);
                vd.push_back(tailSeconds);
                vd.push_back(tailMode);
                totalParticles++;
            };

            const bool drawHead = !vanillaRendering_ || em.headOrTail != 1;
            const bool drawTail = vanillaRendering_ && em.headOrTail >= 1;

            if (drawHead)
                appendRecord(headTileIndex, spinAngle, 0.0f, 0.0f);

            if (drawTail) {
                const float tailTileIndex =
                    cachedTotalTiles > 1
                        ? sampleCell(em.tailCellBegin, em.tailCellEnd)
                        : 0.0f;
                float tailSeconds = std::max(0.0f, em.tailTime);
                if ((em.flags & 0x400u) != 0)
                    tailSeconds = std::min(tailSeconds, std::max(0.0f, p.life));
                appendRecord(tailTileIndex, 0.0f, tailSeconds, 1.0f);
            }
        }
    }

    // Periodic diagnostic: spell effect particle count
    {
        static uint32_t spellParticleDiagFrame_ = 0;
        if (++spellParticleDiagFrame_ % 300 == 1) {
            size_t spellPtc = 0;
            for (const auto& inst : instances) {
                if (inst.cachedModel && inst.cachedModel->isSpellEffect)
                    spellPtc += inst.particles.size();
            }
            if (spellPtc > 0) {
                LOG_INFO("SpellEffect: rendering ", spellPtc, " spell particles (",
                         totalParticles, " total)");
            }
        }
    }

    if (totalParticles == 0) return;

    // Pack every particle group into a UNIQUE range before recording any draw.
    // The old loop repeatedly memcpy'd to byte 0 and then recorded a draw. GPU
    // execution happens after command recording, so later memcpy calls could
    // replace the data that earlier draw commands were meant to consume.
    struct PackedParticleDraw {
        ParticleGroup* group;
        uint32_t firstInstance;
        uint32_t instanceCount;
    };
    std::vector<PackedParticleDraw> packedDraws;
    packedDraws.reserve(groups.size());

    float* packed = static_cast<float*>(m2ParticleVBMapped_);
    size_t packedCount = 0;
    for (auto& [key, group] : groups) {
        if (group.vertexData.empty()) continue;
        if (packedCount >= MAX_M2_RENDER_PARTICLES) break;

        const size_t groupCount = group.vertexData.size() / 15;
        const size_t count = std::min(
            groupCount, MAX_M2_RENDER_PARTICLES - packedCount);
        if (count == 0) continue;

        memcpy(packed + packedCount * 15, group.vertexData.data(),
               count * 15 * sizeof(float));
        packedDraws.push_back({
            .group = &group,
            .firstInstance = static_cast<uint32_t>(packedCount),
            .instanceCount = static_cast<uint32_t>(count)
        });
        packedCount += count;
    }

    if (packedDraws.empty()) return;
    if (packedCount < totalParticles) {
        static bool warnedParticleFrameCap = false;
        if (!warnedParticleFrameCap) {
            LOG_WARNING("M2 particle render buffer capped at ",
                        MAX_M2_RENDER_PARTICLES, " of ", totalParticles,
                        " visible particles; extra particles are not submitted");
            warnedParticleFrameCap = true;
        }
    }

    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            particlePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    VkDeviceSize vbOffset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &m2ParticleVB_, &vbOffset);

    VkPipeline currentPipeline = VK_NULL_HANDLE;

    for (const auto& draw : packedDraws) {
        auto& group = *draw.group;
        const uint8_t blendType = group.blendType;

        VkPipeline desiredPipeline = particlePipeline_;
        switch (blendType) {
            case 0:
            case 1: desiredPipeline = particleOpaquePipeline_; break;
            case 2: desiredPipeline = particlePipeline_; break;
            case 3:
            case 4: desiredPipeline = particleAdditivePipeline_; break;
            case 5: desiredPipeline = particleModulatePipeline_; break;
            case 6: desiredPipeline = particleModulate2xPipeline_; break;
            default: break;
        }
        if (desiredPipeline != currentPipeline) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              desiredPipeline);
            currentPipeline = desiredPipeline;
        }

        // Use the emitter's stable descriptor when available. The fallback is
        // retained for malformed/legacy uploads that did not preallocate one.
        VkDescriptorSet texSet = group.preAllocSet;
        if (texSet == VK_NULL_HANDLE) {
            VkDescriptorSetAllocateInfo ai{
                .sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
            ai.descriptorPool = materialDescPool_;
            ai.descriptorSetCount = 1;
            ai.pSetLayouts = &particleTexLayout_;
            if (vkAllocateDescriptorSets(
                    vkCtx_->getDevice(), &ai, &texSet) == VK_SUCCESS) {
                VkTexture* tex = (group.texture && group.texture->isValid())
                    ? group.texture : whiteTexture_.get();
                if (!tex || !tex->isValid()) continue;
                VkDescriptorImageInfo imgInfo = tex->descriptorInfo();
                VkWriteDescriptorSet write{
                    .sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
                write.dstSet = texSet;
                write.dstBinding = 0;
                write.descriptorCount = 1;
                write.descriptorType =
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                write.pImageInfo = &imgInfo;
                vkUpdateDescriptorSets(
                    vkCtx_->getDevice(), 1, &write, 0, nullptr);
            }
        }
        if (texSet == VK_NULL_HANDLE) continue;

        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                particlePipelineLayout_, 1, 1, &texSet,
                                0, nullptr);

        struct {
            float tileX, tileY;
            int alphaKey;
            int vanillaRendering;
        } pc = {
            .tileX = static_cast<float>(group.tilesX),
            .tileY = static_cast<float>(group.tilesY),
            .alphaKey = (blendType == 1) ? 1 : 0,
            .vanillaRendering = vanillaRendering_ ? 1 : 0
        };
        vkCmdPushConstants(cmd, particlePipelineLayout_,
                           VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                           sizeof(pc), &pc);

        // Four generated vertices form one billboard triangle strip. Particle
        // records are instance-rate input; firstInstance addresses this group's
        // immutable slice of the once-packed frame buffer.
        vkCmdDraw(cmd, 4, draw.instanceCount, 0, draw.firstInstance);
    }
}

void M2Renderer::renderSmokeParticles(VkCommandBuffer cmd, VkDescriptorSet perFrameSet) {
    if (smokeParticles.empty() || !smokePipeline_ || !smokeVB_) return;

    // Build vertex data: pos(3) + lifeRatio(1) + size(1) + isSpark(1) per particle
    size_t count = std::min(smokeParticles.size(), static_cast<size_t>(MAX_SMOKE_PARTICLES));
    float* dst = static_cast<float*>(smokeVBMapped_);
    for (size_t i = 0; i < count; i++) {
        const auto& p = smokeParticles[i];
        *dst++ = p.position.x;
        *dst++ = p.position.y;
        *dst++ = p.position.z;
        *dst++ = p.life / p.maxLife;
        *dst++ = p.size;
        *dst++ = p.isSpark;
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, smokePipeline_);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            smokePipelineLayout_, 0, 1, &perFrameSet, 0, nullptr);

    // Push constant: screenHeight
    float screenHeight = static_cast<float>(vkCtx_->getSwapchainExtent().height);
    vkCmdPushConstants(cmd, smokePipelineLayout_, VK_SHADER_STAGE_VERTEX_BIT, 0,
                       sizeof(float), &screenHeight);

    VkDeviceSize offset = 0;
    vkCmdBindVertexBuffers(cmd, 0, 1, &smokeVB_, &offset);
    vkCmdDraw(cmd, static_cast<uint32_t>(count), 1, 0, 0);
}

} // namespace rendering
} // namespace wowee
