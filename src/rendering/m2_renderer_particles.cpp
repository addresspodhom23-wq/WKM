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
#include <glm/gtc/constants.hpp>
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

glm::vec3 rotateAroundAxis(const glm::vec3& v, glm::vec3 axis, float angle) {
    const float axisLen2 = glm::dot(axis, axis);
    if (axisLen2 <= 1e-12f) return v;
    axis *= glm::inversesqrt(axisLen2);
    const float cs = std::cos(angle);
    const float sn = std::sin(angle);
    return v * cs + glm::cross(axis, v) * sn +
           axis * glm::dot(axis, v) * (1.0f - cs);
}

glm::vec3 cubicBezier(const std::vector<glm::vec3>& points,
                      size_t segment, float t) {
    const size_t i = segment * 3;
    const float u = 1.0f - t;
    return u * u * u * points[i] +
           3.0f * u * u * t * points[i + 1] +
           3.0f * u * t * t * points[i + 2] +
           t * t * t * points[i + 3];
}

glm::vec3 cubicBezierTangent(const std::vector<glm::vec3>& points,
                             size_t segment, float t) {
    const size_t i = segment * 3;
    const float u = 1.0f - t;
    glm::vec3 tangent =
        3.0f * u * u * (points[i + 1] - points[i]) +
        6.0f * u * t * (points[i + 2] - points[i + 1]) +
        3.0f * t * t * (points[i + 3] - points[i + 2]);
    const float len2 = glm::dot(tangent, tangent);
    return len2 > 1e-12f
        ? tangent * glm::inversesqrt(len2)
        : glm::vec3(0.0f, 0.0f, 1.0f);
}

bool sampleClassicSpline(const std::vector<glm::vec3>& points,
                         float normalized,
                         glm::vec3& position,
                         glm::vec3& tangent) {
    if (points.size() < 4) return false;
    const size_t segments = (points.size() - 1) / 3;
    if (segments == 0) return false;

    constexpr int kChords = 16;
    std::vector<float> lengths(segments * kChords + 1, 0.0f);
    glm::vec3 previous = points.front();
    float total = 0.0f;
    for (size_t s = 0; s < segments; ++s) {
        for (int chord = 1; chord <= kChords; ++chord) {
            const glm::vec3 at =
                cubicBezier(points, s, static_cast<float>(chord) / kChords);
            total += glm::length(at - previous);
            lengths[s * kChords + chord] = total;
            previous = at;
        }
    }

    if (total <= 1e-6f) {
        position = points.front();
        tangent = glm::vec3(0.0f, 0.0f, 1.0f);
        return true;
    }

    const float target = glm::clamp(normalized, 0.0f, 1.0f) * total;
    size_t hi = 1;
    while (hi < lengths.size() && lengths[hi] < target) ++hi;
    hi = std::min(hi, lengths.size() - 1);
    const size_t lo = hi - 1;
    const float span = lengths[hi] - lengths[lo];
    const float chordT = span > 1e-6f
        ? (target - lengths[lo]) / span : 0.0f;
    const size_t segment =
        std::min(segments - 1, lo / static_cast<size_t>(kChords));
    const float local =
        (static_cast<float>(lo % kChords) + chordT) / kChords;
    position = cubicBezier(points, segment, local);
    tangent = cubicBezierTangent(points, segment, local);
    return true;
}

float firstTrackFloat(const pipeline::M2AnimationTrack& track,
                      float fallback = 0.0f) {
    for (const auto& sequence : track.sequences) {
        if (!sequence.floatValues.empty())
            return sequence.floatValues.front();
    }
    return fallback;
}

struct ClassicLocalBirth {
    glm::vec3 offset{0.0f};
    glm::vec3 direction{0.0f, 0.0f, 1.0f};
};

ClassicLocalBirth sampleClassicLocalBirth(
        const pipeline::M2ParticleEmitter& emitter,
        float verticalRange,
        float horizontalRange,
        float areaLength,
        float areaWidth,
        float zSource,
        std::mt19937& rng) {
    std::uniform_real_distribution<float> dist01(0.0f, 1.0f);
    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);

    ClassicLocalBirth out;
    bool splineSampled = false;

    if (emitter.emitterType == 3 && emitter.splinePoints.size() >= 4) {
        const float start = glm::clamp(areaLength, 0.0f, 1.0f);
        const float finish = glm::clamp(areaWidth, 0.0f, 1.0f);
        const float t = start + dist01(rng) * (finish - start);
        glm::vec3 tangent(0.0f, 0.0f, 1.0f);
        splineSampled = sampleClassicSpline(
            emitter.splinePoints, t, out.offset, tangent);
        if (splineSampled) {
            if (zSource != 0.0f) {
                out.direction =
                    out.offset - glm::vec3(0.0f, 0.0f, zSource);
                const float d2 = glm::dot(out.direction, out.direction);
                out.direction = d2 > 1e-12f
                    ? out.direction * glm::inversesqrt(d2)
                    : glm::vec3(0.0f, 0.0f, 1.0f);
            } else if (verticalRange != 0.0f) {
                out.direction = rotateAroundAxis(
                    glm::vec3(0.0f, 0.0f, 1.0f),
                    tangent, distN(rng) * verticalRange);
                if (horizontalRange != 0.0f)
                    out.offset +=
                        dist01(rng) * horizontalRange * out.direction;
            } else {
                out.direction = glm::vec3(0.0f);
            }
        }
    }

    if (!splineSampled && emitter.emitterType == 2) {
        const float inner = std::min(areaLength, areaWidth);
        const float outer = std::max(areaLength, areaWidth);
        const float radius =
            inner + dist01(rng) * std::max(0.0f, outer - inner);
        const float latitude = distN(rng) * verticalRange;
        const float longitude = distN(rng) * horizontalRange;
        const float clat = std::cos(latitude);
        const glm::vec3 shell(
            clat * std::cos(longitude),
            clat * std::sin(longitude),
            std::sin(latitude));
        out.offset = shell * radius;
        if (zSource != 0.0f) {
            out.direction =
                out.offset - glm::vec3(0.0f, 0.0f, zSource);
            const float d2 = glm::dot(out.direction, out.direction);
            out.direction = d2 > 1e-12f
                ? out.direction * glm::inversesqrt(d2)
                : glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            out.direction = (emitter.flags & 0x100u)
                ? glm::vec3(0.0f, 0.0f, 1.0f)
                : shell;
        }
    } else if (!splineSampled && emitter.emitterType != 2) {
        out.offset = glm::vec3(
            areaLength * 0.5f * distN(rng),
            areaWidth * 0.5f * distN(rng),
            0.0f);
        if (zSource != 0.0f) {
            out.direction =
                out.offset - glm::vec3(0.0f, 0.0f, zSource);
            const float d2 = glm::dot(out.direction, out.direction);
            out.direction = d2 > 1e-12f
                ? out.direction * glm::inversesqrt(d2)
                : glm::vec3(0.0f, 0.0f, 1.0f);
        } else {
            const float polar = distN(rng) * verticalRange;
            const float azimuth = distN(rng) * horizontalRange;
            const float sinPolar = std::sin(polar);
            out.direction = glm::vec3(
                sinPolar * std::cos(azimuth),
                sinPolar * std::sin(azimuth),
                std::cos(polar));
        }
    }

    // Classic emitter kernel applies +90 degrees about local Z.
    const auto rot90Z = [](const glm::vec3& v) {
        return glm::vec3(-v.y, v.x, v.z);
    };
    out.offset = rot90Z(out.offset);
    out.direction = rot90Z(out.direction);
    return out;
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

    const float simDt = vanillaRendering_
        ? std::min(std::max(dt, 0.0f), 0.1f)
        : dt;

    if (inst.emitterAccumulators.size() != gpu.particleEmitters.size()) {
        inst.emitterAccumulators.resize(gpu.particleEmitters.size(), 0.0f);
    }
    if (inst.particleEmitterGatePrev.size() != gpu.particleEmitters.size()) {
        inst.particleEmitterGatePrev.resize(gpu.particleEmitters.size(), 0);
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
            inst.particleEmitterGatePrev[ei] = 0;
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
            if (vanillaRendering_)
                inst.particleEmitterGatePrev[ei] = 0;
            continue;
        }

        if (vanillaRendering_ && (em.flags & 0x8000u) != 0) {
            // Classic burst emitters interpret rate as a COUNT and fire only
            // on the rising edge of the enabled/rate gate.
            if (inst.particleEmitterGatePrev[ei] == 0)
                inst.emitterAccumulators[ei] =
                    std::floor(std::max(0.0f, rate));
            else
                inst.emitterAccumulators[ei] = 0.0f;
            inst.particleEmitterGatePrev[ei] = 1;
        } else {
            inst.emitterAccumulators[ei] += rate * simDt;
        }

        while (inst.emitterAccumulators[ei] >= 1.0f && inst.particles.size() < MAX_M2_PARTICLES) {
            inst.emitterAccumulators[ei] -= 1.0f;

            M2Particle p;
            p.emitterIndex = static_cast<int>(ei);
            p.life = 0.0f;
            p.maxLife = life;
            p.tileIndex = 0.0f;
            p.phase = particleRng_() & 0x7Fu;

            if (vanillaRendering_ && !em.geometryModel.empty()) {
                // Geometry particles start with the same +90deg local-Z frame
                // used by the Classic emitter kernel, then tumble in their
                // authored local axes.
                p.orientation = glm::angleAxis(
                    glm::half_pi<float>(), glm::vec3(0.0f, 0.0f, 1.0f));
                const glm::vec3 range =
                    em.angularVelocityMax - em.angularVelocityMin;
                p.angularVelocity = glm::vec3(
                    em.angularVelocityMin.x + dist01(particleRng_) * range.x,
                    em.angularVelocityMin.y + dist01(particleRng_) * range.y,
                    em.angularVelocityMin.z + dist01(particleRng_) * range.z);
                if ((em.flags & 0x200u) != 0) {
                    if ((particleRng_() & 1u) == 0u) p.angularVelocity.x = -p.angularVelocity.x;
                    if ((particleRng_() & 1u) == 0u) p.angularVelocity.y = -p.angularVelocity.y;
                    if ((particleRng_() & 1u) == 0u) p.angularVelocity.z = -p.angularVelocity.z;
                }
            }

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
                if (em.emitterType == 3 && em.splinePoints.size() >= 4) {
                    const float splineStart = glm::clamp(areaLength, 0.0f, 1.0f);
                    const float splineEnd = glm::clamp(areaWidth, 0.0f, 1.0f);
                    const float splineT =
                        splineStart + dist01(particleRng_) *
                        (splineEnd - splineStart);
                    glm::vec3 tangent(0.0f, 0.0f, 1.0f);
                    if (sampleClassicSpline(
                            em.splinePoints, splineT,
                            emissionOffset, tangent)) {
                        if (zSource != 0.0f) {
                            dir = emissionOffset -
                                  glm::vec3(0.0f, 0.0f, zSource);
                            const float d2 = glm::dot(dir, dir);
                            dir = d2 > 1e-12f
                                ? dir * glm::inversesqrt(d2)
                                : glm::vec3(0.0f, 0.0f, 1.0f);
                        } else if (vRange != 0.0f) {
                            dir = rotateAroundAxis(
                                glm::vec3(0.0f, 0.0f, 1.0f),
                                tangent, distN(particleRng_) * vRange);
                            if (hRange != 0.0f)
                                emissionOffset +=
                                    dist01(particleRng_) * hRange * dir;
                        } else {
                            dir = glm::vec3(0.0f);
                        }
                    }
                } else if (em.emitterType == 2) {
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
                p.emitterOrigin = em.position;
            } else {
                p.position = glm::vec3(
                    inst.modelMatrix * boneXform * glm::vec4(localPos, 1.0f));
                p.velocity = rotMat * dir * speed;
                p.emitterOrigin = glm::vec3(
                    inst.modelMatrix * boneXform *
                    glm::vec4(em.position, 1.0f));
            }

            if (vanillaRendering_ && !em.geometryModel.empty() && !modelSpace) {
                glm::mat3 basis(inst.modelMatrix * boneXform);
                for (int col = 0; col < 3; ++col) {
                    const float len = glm::length(basis[col]);
                    if (len > 1e-8f) basis[col] /= len;
                }
                p.orientation =
                    glm::normalize(glm::quat_cast(basis) * p.orientation);
            }

            // Classic flag 0x40 samples emitter motion at 30 Hz and adds the
            // held world velocity to newly born particles. The speed-variation
            // factor is sampled independently for this inherited component.
            if (vanillaRendering_ && (em.flags & 0x40u) != 0 &&
                ei < inst.particleInheritVelocities.size()) {
                glm::vec3 inherited = inst.particleInheritVelocities[ei];
                const float inheritFactor =
                    1.0f + speedVariation * distN(particleRng_);
                if (modelSpace) {
                    const glm::mat3 liveFrame(inst.modelMatrix * boneXform);
                    const float det = glm::determinant(liveFrame);
                    if (std::isfinite(det) && std::abs(det) > 1e-8f)
                        inherited = glm::inverse(liveFrame) * inherited;
                    else
                        inherited = glm::vec3(0.0f);
                }
                p.velocity += inheritFactor * inherited;
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
    const size_t numEm = gpu.particleEmitters.size();
    const float simDt = vanillaRendering_
        ? std::min(std::max(dt, 0.0f), 0.1f)
        : dt;

    if (inst.particleEmitterPrevOrigins.size() != numEm)
        inst.particleEmitterPrevOrigins.resize(numEm, glm::vec3(0.0f));
    if (inst.particleEmitterOriginValid.size() != numEm)
        inst.particleEmitterOriginValid.resize(numEm, 0);
    if (inst.particleInheritAccumulators.size() != numEm)
        inst.particleInheritAccumulators.resize(numEm, 0.0f);
    if (inst.particleInheritVelocities.size() != numEm)
        inst.particleInheritVelocities.resize(numEm, glm::vec3(0.0f));

    std::vector<uint8_t> liveByEmitter(numEm, 0);
    for (const auto& p : inst.particles) {
        if (p.emitterIndex >= 0 &&
            static_cast<size_t>(p.emitterIndex) < numEm)
            liveByEmitter[static_cast<size_t>(p.emitterIndex)] = 1;
    }

    constexpr size_t kMaxStackEmitters = 16;
    float emitterGravStack[kMaxStackEmitters];
    std::vector<float> emitterGravHeap;
    float* emitterGrav = nullptr;
    if (numEm > 0) {
        if (numEm <= kMaxStackEmitters) {
            emitterGrav = emitterGravStack;
        } else {
            emitterGravHeap.resize(numEm);
            emitterGrav = emitterGravHeap.data();
        }
    }

    std::vector<glm::vec3> followCorrection(numEm, glm::vec3(0.0f));

    for (size_t e = 0; e < numEm; ++e) {
        const auto& pem = gpu.particleEmitters[e];
        float grav = interpFloat(
            pem.gravity, inst.animTime, inst.globalSequenceTime,
            inst.currentSequenceIndex, gpu.globalSequenceDurations);
        if (!vanillaRendering_ && grav == 0.0f && !gpu.isFireflyEffect) {
            const float emSpeed = interpFloat(
                pem.emissionSpeed, inst.animTime, inst.globalSequenceTime,
                inst.currentSequenceIndex, gpu.globalSequenceDurations);
            grav = (std::abs(emSpeed) > 0.1f) ? 4.0f : 1.5f;
        }
        emitterGrav[e] = grav;

        if (!vanillaRendering_) continue;

        glm::mat4 boneXform(1.0f);
        if (pem.bone < inst.boneMatrices.size())
            boneXform = inst.boneMatrices[pem.bone];
        const glm::mat4 liveFrame4 = inst.modelMatrix * boneXform;
        const glm::vec3 currentOrigin = glm::vec3(
            liveFrame4 * glm::vec4(pem.position, 1.0f));
        const glm::vec3 emitterDelta =
            inst.particleEmitterOriginValid[e]
                ? currentOrigin - inst.particleEmitterPrevOrigins[e]
                : glm::vec3(0.0f);
        inst.particleEmitterPrevOrigins[e] = currentOrigin;
        inst.particleEmitterOriginValid[e] = 1;

        // Flag 0x4000: follow correction is a speed-dependent fraction of the
        // emitter's frame-to-frame translation. Model-space storage already
        // rides the emitter, so its correction is (fraction - 1) * delta.
        if ((pem.flags & 0x4000u) != 0 && simDt > 0.0f &&
            glm::dot(emitterDelta, emitterDelta) > 0.0f &&
            std::abs(pem.followSpeed2 - pem.followSpeed1) >= 1e-6f) {
            const float slope =
                (pem.followScale2 - pem.followScale1) /
                (pem.followSpeed2 - pem.followSpeed1);
            const float intercept =
                pem.followScale1 - slope * pem.followSpeed1;
            const float fraction = glm::clamp(
                slope * glm::length(emitterDelta) / simDt + intercept,
                0.0f, 1.0f);
            const bool modelSpace = (pem.flags & 0x10u) != 0;
            glm::vec3 correction =
                (modelSpace ? fraction - 1.0f : fraction) * emitterDelta;
            if (modelSpace) {
                const glm::mat3 linear(liveFrame4);
                const float det = glm::determinant(linear);
                correction = (std::isfinite(det) && std::abs(det) > 1e-8f)
                    ? glm::inverse(linear) * correction
                    : glm::vec3(0.0f);
            }
            followCorrection[e] = correction;
        }

        // Flag 0x40: 30 Hz sample-and-hold inherited velocity. Until this
        // emitter already owns a live particle the held value is zero.
        if ((pem.flags & 0x40u) != 0) {
            float& accumulator = inst.particleInheritAccumulators[e];
            accumulator += simDt;
            constexpr float kInheritInterval = 1.0f / 30.0f;
            if (accumulator > kInheritInterval) {
                inst.particleInheritVelocities[e] = liveByEmitter[e]
                    ? emitterDelta *
                        (kInheritInterval / accumulator) * pem.inheritScale
                    : glm::vec3(0.0f);
                accumulator = 0.0f;
            }
        } else {
            inst.particleInheritAccumulators[e] = 0.0f;
            inst.particleInheritVelocities[e] = glm::vec3(0.0f);
        }
    }

    for (size_t i = 0; i < inst.particles.size(); ) {
        auto& p = inst.particles[i];
        p.life += simDt;
        if (p.life >= p.maxLife) {
            inst.particles[i] = inst.particles.back();
            inst.particles.pop_back();
            continue;
        }

        if (p.emitterIndex >= 0 &&
            static_cast<size_t>(p.emitterIndex) < numEm) {
            const size_t emitterIndex =
                static_cast<size_t>(p.emitterIndex);
            const auto& em = gpu.particleEmitters[emitterIndex];
            const float grav = emitterGrav[emitterIndex];
            const bool modelSpace =
                vanillaRendering_ && ((em.flags & 0x10u) != 0);

            if (vanillaRendering_)
                p.position += followCorrection[emitterIndex];

            const glm::vec3 stepVelocity = p.velocity;

            if (vanillaRendering_ && !em.geometryModel.empty()) {
                const float angularSpeed = glm::length(p.angularVelocity);
                if (angularSpeed > 1e-8f) {
                    const glm::quat delta = glm::angleAxis(
                        angularSpeed * simDt,
                        p.angularVelocity / angularSpeed);
                    p.orientation = glm::normalize(p.orientation * delta);
                }
            }

            if (modelSpace) {
                p.position += p.velocity * simDt;
                if (grav != 0.0f) {
                    p.position.z -= 0.5f * grav * simDt * simDt;
                    p.velocity.z -= grav * simDt;
                }
                if (em.drag > 0.0f) {
                    const float drag =
                        std::min(simDt * em.drag, 1.0f);
                    p.velocity -= drag * p.velocity;
                }
            } else if (vanillaRendering_) {
                // Vanilla advances with pre-gravity velocity, then applies the
                // closed-form half-step. Long frames use the same 0.1 s clamp
                // as emission/follow/inherit.
                p.position += p.velocity * simDt;
                if (grav != 0.0f) {
                    p.position.z -= 0.5f * grav * simDt * simDt;
                    p.velocity.z -= grav * simDt;
                }
                if (em.drag > 0.0f) {
                    const float drag =
                        std::min(simDt * em.drag, 1.0f);
                    p.velocity -= drag * p.velocity;
                }
            } else {
                p.velocity.z -= grav * dt;
                p.position += p.velocity * dt;
            }

            if (vanillaRendering_ && em.emitterType == 2 &&
                (em.flags & 0x80u) != 0 &&
                glm::dot(stepVelocity,
                         p.position - p.emitterOrigin) > 0.0f) {
                inst.particles[i] = inst.particles.back();
                inst.particles.pop_back();
                continue;
            }
        } else {
            p.position += p.velocity * simDt;
        }
        ++i;
    }
}

void M2Renderer::updateRecursiveParticles(
        M2Instance& inst, const M2ModelGPU& gpu, float dt) {
    if (!vanillaRendering_ || inst.forcedHidden)
        return;

    const float simDt = std::min(std::max(dt, 0.0f), 0.1f);
    if (!(simDt > 0.0f))
        return;

    auto canonicalModelPath = [](std::string path) {
        path = pipeline::modelPathToM2(path);
        std::replace(path.begin(), path.end(), '/', '\\');
        std::transform(path.begin(), path.end(), path.begin(),
                       [](unsigned char ch) {
                           return static_cast<char>(std::tolower(ch));
                       });
        return path;
    };

    // Wire each parent emitter to the recursion model's first four usable
    // child emitters. State is private to this parent instance/emitter pair.
    for (size_t parentIndex = 0;
         parentIndex < gpu.particleEmitters.size();
         ++parentIndex) {
        const auto& parentEmitter = gpu.particleEmitters[parentIndex];
        if (parentEmitter.recursionModel.empty())
            continue;

        const std::string key =
            canonicalModelPath(parentEmitter.recursionModel);
        const auto runtimeIdIt = particleRecursionModelIds_.find(key);
        if (runtimeIdIt == particleRecursionModelIds_.end())
            continue;
        const uint32_t runtimeId = runtimeIdIt->second;
        const auto runtimeIt = particleRecursionModels_.find(runtimeId);
        if (runtimeIt == particleRecursionModels_.end())
            continue;

        for (uint16_t childIndex :
             runtimeIt->second.validEmitterIndices) {
            const auto stateIt = std::find_if(
                inst.recursiveEmitterStates.begin(),
                inst.recursiveEmitterStates.end(),
                [&](const M2RecursiveEmitterState& state) {
                    return state.runtimeId == runtimeId &&
                           state.parentEmitterIndex == parentIndex &&
                           state.childEmitterIndex == childIndex;
                });
            if (stateIt == inst.recursiveEmitterStates.end()) {
                inst.recursiveEmitterStates.push_back({
                    .runtimeId = runtimeId,
                    .parentEmitterIndex =
                        static_cast<uint16_t>(parentIndex),
                    .childEmitterIndex = childIndex,
                    .accumulator = 0.0f,
                    .gatePrev = 0
                });
            }
        }
    }

    std::uniform_real_distribution<float> distN(-1.0f, 1.0f);

    // Parent integration/emission has already completed this frame. Each child
    // emitter now observes the complete post-birth parent pool, exactly as the
    // Classic recursion law does.
    for (auto& state : inst.recursiveEmitterStates) {
        if (state.parentEmitterIndex >= gpu.particleEmitters.size())
            continue;
        const auto runtimeIt =
            particleRecursionModels_.find(state.runtimeId);
        if (runtimeIt == particleRecursionModels_.end())
            continue;
        const auto& runtime = runtimeIt->second;
        if (state.childEmitterIndex >=
            runtime.model.particleEmitters.size())
            continue;

        const auto& parentEmitter =
            gpu.particleEmitters[state.parentEmitterIndex];
        const auto& childEmitter =
            runtime.model.particleEmitters[state.childEmitterIndex];

        const float baseRate = firstTrackFloat(
            childEmitter.emissionRate, 0.0f);
        const float rate = std::max(
            0.0f, m2_track::sampleFloat(
                childEmitter.emissionRate, 0,
                inst.animTime, inst.globalSequenceTime,
                runtime.model.globalSequenceDurations,
                baseRate));
        const float enabled = m2_track::sampleFloat(
            childEmitter.visibilityTrack, 0,
            inst.animTime, inst.globalSequenceTime,
            runtime.model.globalSequenceDurations, 1.0f);
        const bool emitting = enabled > 0.0f && rate > 0.0f;

        const float speedBase =
            firstTrackFloat(childEmitter.emissionSpeed, 0.0f);
        const float speedVariation =
            firstTrackFloat(childEmitter.speedVariation, 0.0f);
        const float verticalRange =
            firstTrackFloat(childEmitter.verticalRange, 0.0f);
        const float horizontalRange =
            firstTrackFloat(childEmitter.horizontalRange, 0.0f);
        const float life =
            firstTrackFloat(childEmitter.lifespan, 0.0f);
        const float areaLength =
            firstTrackFloat(childEmitter.emissionAreaLength, 0.0f);
        const float areaWidth =
            firstTrackFloat(childEmitter.emissionAreaWidth, 0.0f);
        const float zSource =
            firstTrackFloat(childEmitter.zSource, 0.0f);

        if (!(life > 0.0f))
            continue;

        glm::mat4 parentFrame = inst.modelMatrix;
        if (parentEmitter.bone < inst.boneMatrices.size())
            parentFrame *= inst.boneMatrices[parentEmitter.bone];
        const glm::mat3 parentLinear(parentFrame);
        const bool modelSpace =
            (parentEmitter.flags & 0x10u) != 0;

        for (const auto& parentParticle : inst.particles) {
            if (parentParticle.emitterIndex !=
                static_cast<int>(state.parentEmitterIndex))
                continue;

            if (!emitting)
                state.accumulator = 0.0f;
            if ((childEmitter.flags & 0x8000u) != 0) {
                if (emitting && state.gatePrev == 0)
                    state.accumulator =
                        std::floor(rate);
            } else if (emitting) {
                state.accumulator += rate * simDt;
            }
            const uint8_t gateNow = emitting ? 1 : 0;
            state.gatePrev = gateNow;

            while (state.accumulator >= 1.0f &&
                   inst.recursiveParticles.size() <
                       MAX_M2_PARTICLES) {
                state.accumulator -= 1.0f;

                const ClassicLocalBirth local =
                    sampleClassicLocalBirth(
                        childEmitter, verticalRange,
                        horizontalRange, areaLength,
                        areaWidth, zSource, particleRng_);

                const float speed =
                    speedBase *
                    (1.0f + speedVariation *
                        distN(particleRng_));

                M2RecursiveParticle recursive;
                recursive.runtimeId = state.runtimeId;
                recursive.parentEmitterIndex =
                    state.parentEmitterIndex;
                recursive.childEmitterIndex =
                    state.childEmitterIndex;

                auto& particle = recursive.particle;
                particle.emitterIndex =
                    static_cast<int>(state.childEmitterIndex);
                particle.life = 0.0f;
                particle.maxLife = life;
                particle.tileIndex = 0.0f;
                particle.phase = particleRng_() & 0x7Fu;
                particle.orientation =
                    glm::quat(1.0f, 0.0f, 0.0f, 0.0f);
                particle.angularVelocity = glm::vec3(0.0f);

                if (modelSpace) {
                    particle.position =
                        parentParticle.position + local.offset;
                    particle.velocity =
                        local.direction * speed;
                } else {
                    particle.position =
                        parentParticle.position +
                        parentLinear * local.offset;
                    particle.velocity =
                        parentLinear * local.direction * speed;
                }

                if ((childEmitter.flags & 0x40u) != 0) {
                    const float inheritedFactor =
                        1.0f + speedVariation *
                            distN(particleRng_);
                    particle.velocity +=
                        inheritedFactor *
                        parentParticle.velocity;
                }

                particle.emitterOrigin =
                    parentParticle.position;
                inst.recursiveParticles.push_back(
                    std::move(recursive));
            }
        }
    }

    // Unlike parent particles, recursion children born above are integrated on
    // this same frame.
    for (size_t i = 0;
         i < inst.recursiveParticles.size();) {
        auto& recursive = inst.recursiveParticles[i];
        const auto runtimeIt =
            particleRecursionModels_.find(recursive.runtimeId);
        if (runtimeIt == particleRecursionModels_.end() ||
            recursive.childEmitterIndex >=
                runtimeIt->second.model.particleEmitters.size() ||
            recursive.parentEmitterIndex >=
                gpu.particleEmitters.size()) {
            inst.recursiveParticles[i] =
                inst.recursiveParticles.back();
            inst.recursiveParticles.pop_back();
            continue;
        }

        const auto& childEmitter =
            runtimeIt->second.model.particleEmitters[
                recursive.childEmitterIndex];
        auto& particle = recursive.particle;
        particle.life += simDt;
        if (particle.life >= particle.maxLife) {
            inst.recursiveParticles[i] =
                inst.recursiveParticles.back();
            inst.recursiveParticles.pop_back();
            continue;
        }

        const float gravity =
            firstTrackFloat(childEmitter.gravity, 0.0f);
        particle.position += particle.velocity * simDt;
        if (gravity != 0.0f) {
            particle.position.z -=
                0.5f * gravity * simDt * simDt;
            particle.velocity.z -= gravity * simDt;
        }
        if (childEmitter.drag > 0.0f) {
            const float drag =
                std::min(simDt * childEmitter.drag, 1.0f);
            particle.velocity -= drag * particle.velocity;
        }

        ++i;
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
        g.submissionOrder = std::numeric_limits<uint64_t>::max();
    }
    auto& groups = particleGroups_;

    size_t totalParticles = 0;

    uint64_t particleInstanceOrder = 0;
    for (auto& inst : instances) {
        const uint64_t instanceSortBase =
            (particleInstanceOrder++) << 32u;
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
        uint8_t cachedFogPolicy = 1;
        glm::vec3 cachedPlaneRight(0.0f);
        glm::vec3 cachedPlaneUp(0.0f);
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
                cachedFogPolicy = vanillaRendering_
                    ? ((cachedEm->flags & 0x08u) != 0
                        ? 0u
                        : ((cachedBlendType == 3 || cachedBlendType == 4)
                            ? 2u : 1u))
                    : 1u;

                cachedPlaneRight = glm::vec3(0.0f);
                cachedPlaneUp = glm::vec3(0.0f);
                if (vanillaRendering_ &&
                    (cachedEm->flags & 0x1000u) != 0) {
                    glm::mat4 liveFrame = inst.modelMatrix;
                    if (cachedEm->bone < inst.boneMatrices.size())
                        liveFrame *= inst.boneMatrices[cachedEm->bone];
                    glm::vec3 right =
                        glm::mat3(liveFrame) * glm::vec3(0.0f, 1.0f, 0.0f);
                    glm::vec3 up =
                        glm::mat3(liveFrame) * glm::vec3(-1.0f, 0.0f, 0.0f);
                    const float rightLen = glm::length(right);
                    const float upLen = glm::length(up);
                    if (rightLen > 1e-8f && upLen > 1e-8f) {
                        cachedPlaneRight =
                            (right / rightLen) * inst.scale;
                        cachedPlaneUp =
                            (up / upLen) * inst.scale;
                    }
                }

                const uint64_t orderToken = vanillaRendering_
                    ? instanceSortBase +
                        static_cast<uint64_t>(p.emitterIndex) * 8u
                    : 0u;
                ParticleGroupKey key{
                    .texture = cachedTex,
                    .blendType =
                        static_cast<uint8_t>(cachedBlendType),
                    .tilesX = cachedTilesX,
                    .tilesY = cachedTilesY,
                    .fogPolicy = cachedFogPolicy,
                    .orderToken = orderToken
                };
                cachedGroup = &groups[key];
                cachedGroup->texture = cachedTex;
                cachedGroup->blendType = cachedBlendType;
                cachedGroup->tilesX = cachedTilesX;
                cachedGroup->tilesY = cachedTilesY;
                cachedGroup->fogPolicy = cachedFogPolicy;
                cachedGroup->submissionOrder =
                    std::min(cachedGroup->submissionOrder,
                             orderToken);
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
            if (vanillaRendering_ && !em.geometryModel.empty())
                continue;
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
                                    float tailSeconds, float tailMode,
                                    const glm::vec3& planeRight,
                                    const glm::vec3& planeUp) {
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
                vd.push_back(planeRight.x);
                vd.push_back(planeRight.y);
                vd.push_back(planeRight.z);
                vd.push_back(planeUp.x);
                vd.push_back(planeUp.y);
                vd.push_back(planeUp.z);
                totalParticles++;
            };

            const bool drawHead = !vanillaRendering_ || em.headOrTail != 1;
            const bool drawTail = vanillaRendering_ && em.headOrTail >= 1;

            if (drawHead)
                appendRecord(headTileIndex, spinAngle, 0.0f, 0.0f,
                             cachedPlaneRight, cachedPlaneUp);

            if (drawTail) {
                const float tailTileIndex =
                    cachedTotalTiles > 1
                        ? sampleCell(em.tailCellBegin, em.tailCellEnd)
                        : 0.0f;
                float tailSeconds = std::max(0.0f, em.tailTime);
                if ((em.flags & 0x400u) != 0)
                    tailSeconds = std::min(tailSeconds, std::max(0.0f, p.life));
                appendRecord(tailTileIndex, 0.0f, tailSeconds, 1.0f,
                             glm::vec3(0.0f), glm::vec3(0.0f));
            }
        }
    }

    // Recursion children are private particle pools with their own emitter
    // definition/texture/blend state. Even when the recursion record names a
    // geometry model, Classic renders child records as particle quads.
    uint64_t recursiveInstanceOrder = 0;
    for (auto& inst : instances) {
        const uint64_t instanceSortBase =
            (recursiveInstanceOrder++) << 32u;
        if (inst.forcedHidden || inst.recursiveParticles.empty() ||
            !inst.cachedModel)
            continue;

        const auto& parentGpu = *inst.cachedModel;
        for (const auto& recursive : inst.recursiveParticles) {
            const auto runtimeIt =
                particleRecursionModels_.find(recursive.runtimeId);
            if (runtimeIt == particleRecursionModels_.end())
                continue;
            const auto& runtime = runtimeIt->second;
            if (recursive.childEmitterIndex >=
                    runtime.model.particleEmitters.size() ||
                recursive.parentEmitterIndex >=
                    parentGpu.particleEmitters.size())
                continue;

            const auto& em =
                runtime.model.particleEmitters[
                    recursive.childEmitterIndex];
            const auto& parentEmitter =
                parentGpu.particleEmitters[
                    recursive.parentEmitterIndex];
            const auto& p = recursive.particle;

            VkTexture* tex = whiteTexture_.get();
            VkDescriptorSet stableSet = VK_NULL_HANDLE;
            if (recursive.childEmitterIndex <
                runtime.emitterTextures.size()) {
                tex = runtime.emitterTextures[
                    recursive.childEmitterIndex];
            }
            if (recursive.childEmitterIndex <
                runtime.emitterTexSets.size()) {
                stableSet = runtime.emitterTexSets[
                    recursive.childEmitterIndex];
            }

            const uint16_t tilesX =
                std::max<uint16_t>(em.textureCols, 1);
            const uint16_t tilesY =
                std::max<uint16_t>(em.textureRows, 1);
            const uint32_t totalTiles =
                static_cast<uint32_t>(tilesX) *
                static_cast<uint32_t>(tilesY);

            const uint8_t fogPolicy =
                (em.flags & 0x08u) != 0
                    ? 0u
                    : ((em.blendingType == 3 ||
                        em.blendingType == 4) ? 2u : 1u);

            glm::vec3 planeRight(0.0f);
            glm::vec3 planeUp(0.0f);
            if ((em.flags & 0x1000u) != 0) {
                glm::mat4 liveFrame = inst.modelMatrix;
                if (parentEmitter.bone < inst.boneMatrices.size())
                    liveFrame *= inst.boneMatrices[parentEmitter.bone];
                glm::vec3 right =
                    glm::mat3(liveFrame) * glm::vec3(0.0f, 1.0f, 0.0f);
                glm::vec3 up =
                    glm::mat3(liveFrame) * glm::vec3(-1.0f, 0.0f, 0.0f);
                const float rightLen = glm::length(right);
                const float upLen = glm::length(up);
                if (rightLen > 1e-8f && upLen > 1e-8f) {
                    planeRight = (right / rightLen) * inst.scale;
                    planeUp = (up / upLen) * inst.scale;
                }
            }

            const uint64_t orderToken =
                instanceSortBase +
                static_cast<uint64_t>(
                    recursive.parentEmitterIndex) * 8u +
                1u +
                static_cast<uint64_t>(
                    recursive.childEmitterIndex);
            ParticleGroupKey key{
                .texture = tex,
                .blendType =
                    static_cast<uint8_t>(em.blendingType),
                .tilesX = tilesX,
                .tilesY = tilesY,
                .fogPolicy = fogPolicy,
                .orderToken = orderToken
            };
            auto& group = groups[key];
            group.texture = tex;
            group.blendType = em.blendingType;
            group.tilesX = tilesX;
            group.tilesY = tilesY;
            group.fogPolicy = fogPolicy;
            group.submissionOrder =
                std::min(group.submissionOrder,
                         orderToken);
            if (group.preAllocSet == VK_NULL_HANDLE)
                group.preAllocSet = stableSet;

            const float lifeRatio =
                p.life / std::max(p.maxLife, 0.001f);
            const glm::vec3 color =
                interpFBlockVec3(
                    em.particleColor, lifeRatio);
            const float alpha = std::min(
                interpFBlockFloat(
                    em.particleAlpha, lifeRatio),
                1.0f);
            float scale =
                interpFBlockFloat(
                    em.particleScale, lifeRatio);

            const float twinkleNoise =
                vanillaTwinkleNoise(
                    em.twinkleSpeed, p.life, p.phase);
            if (em.twinklePercent < 1.0f &&
                twinkleNoise > em.twinklePercent)
                continue;
            if (std::abs(
                    em.twinkleMax - em.twinkleMin) >= 1e-6f) {
                scale *= glm::mix(
                    em.twinkleMin, em.twinkleMax,
                    twinkleNoise);
            }
            if ((em.flags & 0x20u) != 0)
                scale *= inst.scale;

            glm::vec3 drawPos = p.position;
            glm::vec3 drawVelocity = p.velocity;
            const bool modelSpace =
                (parentEmitter.flags & 0x10u) != 0;
            if (modelSpace) {
                glm::mat4 liveFrame = inst.modelMatrix;
                if (parentEmitter.bone <
                    inst.boneMatrices.size()) {
                    liveFrame *=
                        inst.boneMatrices[
                            parentEmitter.bone];
                }
                drawPos = glm::vec3(
                    liveFrame *
                    glm::vec4(p.position, 1.0f));
                drawVelocity =
                    glm::mat3(liveFrame) * p.velocity;
            }

            const float tLife =
                glm::clamp(lifeRatio, 0.0f, 1.0f);
            const float mid =
                glm::clamp(
                    em.lifeMidpoint, 0.001f, 1.0f);
            const int seg = tLife <= mid ? 0 : 1;
            float segT = seg == 0
                ? tLife / mid
                : (tLife - mid) /
                    std::max(1.0f - mid, 0.001f);
            segT = glm::clamp(segT, 0.0f, 1.0f) *
                   0.99f + 0.005f;
            const float repeat =
                static_cast<float>(
                    em.headCellRepeat[seg]);
            const float cellT = repeat != 1.0f
                ? segT * repeat -
                    std::floor(segT * repeat)
                : segT;

            auto sampleCell =
                [&](const uint16_t begin[2],
                    const uint16_t finish[2]) {
                    if (totalTiles <= 1)
                        return 0.0f;
                    const int b =
                        static_cast<int>(begin[seg]);
                    const int e =
                        static_cast<int>(finish[seg]);
                    const int baseCell =
                        e >= b ? b : b + 1;
                    const int span =
                        e >= b ? e - b + 1
                               : e - b - 1;
                    const int authoredCell =
                        static_cast<int>(
                            std::floor(
                                baseCell +
                                span * cellT)) &
                        0xFF;
                    return static_cast<float>(
                        static_cast<uint32_t>(
                            authoredCell) %
                        totalTiles);
                };

            float spinAngle = em.spin * p.life;
            if (spinAngle < 0.0f &&
                (p.phase & 0x20u) != 0)
                spinAngle = -spinAngle;

            auto appendRecord =
                [&](float tileIndex, float spin,
                    float tailSeconds,
                    float tailMode,
                    const glm::vec3& headPlaneRight,
                    const glm::vec3& headPlaneUp) {
                    auto& vd = group.vertexData;
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
                    vd.push_back(headPlaneRight.x);
                    vd.push_back(headPlaneRight.y);
                    vd.push_back(headPlaneRight.z);
                    vd.push_back(headPlaneUp.x);
                    vd.push_back(headPlaneUp.y);
                    vd.push_back(headPlaneUp.z);
                    ++totalParticles;
                };

            if (em.headOrTail != 1) {
                appendRecord(
                    sampleCell(
                        em.headCellBegin,
                        em.headCellEnd),
                    spinAngle, 0.0f, 0.0f,
                    planeRight, planeUp);
            }
            if (em.headOrTail >= 1) {
                float tailSeconds =
                    std::max(0.0f, em.tailTime);
                if ((em.flags & 0x400u) != 0) {
                    tailSeconds = std::min(
                        tailSeconds,
                        std::max(0.0f, p.life));
                }
                appendRecord(
                    sampleCell(
                        em.tailCellBegin,
                        em.tailCellEnd),
                    0.0f, tailSeconds, 1.0f,
                    glm::vec3(0.0f), glm::vec3(0.0f));
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
                    spellPtc += inst.particles.size() +
                                inst.recursiveParticles.size();
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

    std::vector<ParticleGroup*> frameGroups;
    frameGroups.reserve(groups.size());
    for (auto& [key, group] : groups) {
        (void)key;
        if (!group.vertexData.empty())
            frameGroups.push_back(&group);
    }
    if (vanillaRendering_) {
        // Retail keeps non-additive emitter ordering barriers. Consecutive
        // additive ranges may be regrouped because their blend is commutative;
        // preserving the authored order for them as well is visually equivalent
        // and, critically, never lets an unordered_map move alpha ranges across
        // another emitter.
        std::stable_sort(
            frameGroups.begin(), frameGroups.end(),
            [](const ParticleGroup* lhs,
               const ParticleGroup* rhs) {
                return lhs->submissionOrder <
                       rhs->submissionOrder;
            });
    }

    float* packed = static_cast<float*>(m2ParticleVBMapped_);
    size_t packedCount = 0;
    for (ParticleGroup* groupPtr : frameGroups) {
        auto& group = *groupPtr;
        if (packedCount >= MAX_M2_RENDER_PARTICLES) break;

        const size_t groupCount = group.vertexData.size() / 21;
        const size_t count = std::min(
            groupCount, MAX_M2_RENDER_PARTICLES - packedCount);
        if (count == 0) continue;

        memcpy(packed + packedCount * 21, group.vertexData.data(),
               count * 21 * sizeof(float));
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
            int fogPolicy;
        } pc = {
            .tileX = static_cast<float>(group.tilesX),
            .tileY = static_cast<float>(group.tilesY),
            .alphaKey = (blendType == 1) ? 1 : 0,
            .vanillaRendering = vanillaRendering_ ? 1 : 0,
            .fogPolicy = static_cast<int>(group.fogPolicy)
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
