// m2_renderer_internal.h - shared helpers for the m2_renderer split files.
// All functions are inline to allow inclusion in multiple translation units.
#pragma once

#include "rendering/collision_geometry.hpp"
#include "rendering/m2_renderer.hpp"
#include "rendering/m2_track_sampler.hpp"
#include "pipeline/m2_loader.hpp"
#include "core/profiler.hpp"
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <glm/gtx/quaternion.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <random>
#include <unordered_set>
#include <vector>

namespace wowee {
namespace rendering {
namespace m2_internal {

// ---- RNG helpers ----
inline std::mt19937& rng() {
    static std::mt19937 gen(std::random_device{}());
    return gen;
}
inline uint32_t randRange(uint32_t maxExclusive) {
    if (maxExclusive == 0) return 0;
    return std::uniform_int_distribution<uint32_t>(0, maxExclusive - 1)(rng());
}
inline float randFloat(float lo, float hi) {
    return std::uniform_real_distribution<float>(lo, hi)(rng());
}

// World-space center of a skinned batch. Glow cards on swinging lanterns are
// bone animated; using only modelMatrix * bind-pose center leaves both their
// sprite and the pool of local light fixed at the fixture origin.
inline glm::vec3 animatedBatchWorldCenter(const M2Instance& instance,
                                          const M2ModelGPU::BatchGPU& batch) {
    if (batch.lightBoneAnchors.empty()) {
        return glm::vec3(instance.modelMatrix * glm::vec4(batch.center, 1.0f));
    }

    glm::vec4 animatedCenter(0.0f);
    for (const auto& anchor : batch.lightBoneAnchors) {
        if (anchor.bone < instance.boneMatrices.size()) {
            animatedCenter += instance.boneMatrices[anchor.bone] * anchor.weightedPoint;
        } else {
            animatedCenter += anchor.weightedPoint;
        }
    }
    return glm::vec3(instance.modelMatrix * animatedCenter);
}

// Lanterns are broad point lights, so the small physical displacement of a
// swinging bulb is almost invisible on the ground. Follow the authored hanging
// chain from its fixed suspension pivot to the animated glow tip, then project
// that direction toward the model's ground plane. The glow sprite remains at
// animatedBatchWorldCenter(); only the pool of light uses this projected center.
inline glm::vec3 animatedBatchLightWorldCenter(const M2Instance& instance,
                                               const M2ModelGPU::BatchGPU& batch) {
    glm::vec3 tip = animatedBatchWorldCenter(instance, batch);
    if (batch.lightSuspensionBone == UINT16_MAX ||
        batch.lightSuspensionBone >= instance.boneMatrices.size()) {
        return tip;
    }

    const glm::vec3 suspension = glm::vec3(
        instance.modelMatrix * instance.boneMatrices[batch.lightSuspensionBone] *
        glm::vec4(batch.lightSuspensionPoint, 1.0f));
    const glm::vec3 chain = tip - suspension;
    if (chain.z >= -0.05f) return tip;

    const float groundZ = glm::vec3(instance.modelMatrix[3]).z;
    const float groundDrop = tip.z - groundZ;
    if (groundDrop <= 0.0f) return tip;

    // Deliberately amplify the projected travel. At the authored Darkshire
    // swing amplitude a physically exact pool moves less than one terrain unit,
    // which disappears inside the broad local-light falloff.
    constexpr float kLanternPoolSwingExaggeration = 3.0f;
    glm::vec2 projectedOffset(chain.x, chain.y);
    projectedOffset *= (groundDrop / -chain.z) * kLanternPoolSwingExaggeration;
    const float offsetLength = glm::length(projectedOffset);
    const float maxOffset = std::max(0.75f, 4.0f * instance.scale);
    if (offsetLength > maxOffset) projectedOffset *= maxOffset / offsetLength;
    tip.x += projectedOffset.x;
    tip.y += projectedOffset.y;
    return tip;
}

// ---- Constants ----
inline const auto kLavaAnimStart = std::chrono::steady_clock::now();
inline constexpr uint32_t kParticleFlagRandomized = 0x40;
inline constexpr uint32_t kParticleFlagTiled = 0x80;
inline constexpr float kSmokeEmitInterval = 1.0f / 48.0f;

// ---- Geometry / collision helpers ----

inline float computeGroundDetailDownOffset(const M2ModelGPU& model, float scale) {
    const float pivotComp = glm::clamp(std::max(0.0f, model.boundMin.z * scale), 0.0f, 0.10f);
    const float terrainSink = 0.03f;
    return pivotComp + terrainSink;
}

inline void getTightCollisionBounds(const M2ModelGPU& model, glm::vec3& outMin, glm::vec3& outMax) {
    // Authored vegetation geometry can be offset from the visual canopy centre
    // and extend beyond the guessed short trunk box. The spatial grid and the
    // broad phase must enclose EVERY triangle tested by the narrow phase.
    if ((model.isFoliageLike || model.collisionTreeTrunk) && model.collision.valid()) {
        outMin = model.collision.boundsMin;
        outMax = model.collision.boundsMax;
        return;
    }

    glm::vec3 center = (model.boundMin + model.boundMax) * 0.5f;
    glm::vec3 half = (model.boundMax - model.boundMin) * 0.5f;

    if (model.collisionTreeTrunk) {
        float modelHoriz = std::max(model.boundMax.x - model.boundMin.x,
                                    model.boundMax.y - model.boundMin.y);
        float trunkHalf = std::clamp(modelHoriz * 0.05f, 0.5f, 5.0f);
        half.x = trunkHalf;
        half.y = trunkHalf;
        half.z = std::min(trunkHalf * 2.5f, 3.5f);
        center.z = model.boundMin.z + half.z;
    } else if (model.collisionNarrowVerticalProp) {
        half.x *= 0.30f;
        half.y *= 0.30f;
        half.z *= 0.96f;
    } else if (model.collisionSmallSolidProp) {
        half.x *= 1.00f;
        half.y *= 1.00f;
        half.z *= 1.00f;
    } else if (model.collisionSteppedLowPlatform) {
        half.x *= 0.98f;
        half.y *= 0.98f;
        half.z *= 0.52f;
    } else {
        half.x *= 0.66f;
        half.y *= 0.66f;
        half.z *= 0.76f;
    }

    outMin = center - half;
    outMax = center + half;
}

inline float getEffectiveCollisionTopLocal(const M2ModelGPU& model,
                                           const glm::vec3& localPos,
                                           const glm::vec3& localMin,
                                           const glm::vec3& localMax) {
    if (!model.collisionSteppedFountain && !model.collisionSteppedLowPlatform) {
        return localMax.z;
    }

    glm::vec2 center((localMin.x + localMax.x) * 0.5f, (localMin.y + localMax.y) * 0.5f);
    glm::vec2 half((localMax.x - localMin.x) * 0.5f, (localMax.y - localMin.y) * 0.5f);
    if (half.x < 1e-4f || half.y < 1e-4f) {
        return localMax.z;
    }

    float nx = (localPos.x - center.x) / half.x;
    float ny = (localPos.y - center.y) / half.y;
    float r = std::sqrt(nx * nx + ny * ny);

    float h = localMax.z - localMin.z;
    if (model.collisionSteppedFountain) {
        if (r > 0.85f) return localMin.z + h * 0.18f;
        if (r > 0.65f) return localMin.z + h * 0.36f;
        if (r > 0.45f) return localMin.z + h * 0.54f;
        if (r > 0.28f) return localMin.z + h * 0.70f;
        if (r > 0.14f) return localMin.z + h * 0.84f;
        return localMin.z + h * 0.96f;
    }

    float edge = std::max(std::abs(nx), std::abs(ny));
    if (edge > 0.92f) return localMin.z + h * 0.06f;
    if (edge > 0.72f) return localMin.z + h * 0.30f;
    return localMin.z + h * 0.62f;
}

inline bool segmentIntersectsAABB(const glm::vec3& from, const glm::vec3& to,
                                  const glm::vec3& bmin, const glm::vec3& bmax,
                                  float& outEnterT) {
    glm::vec3 d = to - from;
    float tEnter = 0.0f;
    float tExit = 1.0f;

    for (int axis = 0; axis < 3; axis++) {
        if (std::abs(d[axis]) < 1e-6f) {
            if (from[axis] < bmin[axis] || from[axis] > bmax[axis]) {
                return false;
            }
            continue;
        }

        float inv = 1.0f / d[axis];
        float t0 = (bmin[axis] - from[axis]) * inv;
        float t1 = (bmax[axis] - from[axis]) * inv;
        if (t0 > t1) std::swap(t0, t1);

        tEnter = std::max(tEnter, t0);
        tExit = std::min(tExit, t1);
        if (tEnter > tExit) return false;
    }

    outEnterT = tEnter;
    return tExit >= 0.0f && tEnter <= 1.0f;
}

inline void transformAABB(const glm::mat4& modelMatrix,
                          const glm::vec3& localMin,
                          const glm::vec3& localMax,
                          glm::vec3& outMin,
                          glm::vec3& outMax) {
    const glm::vec3 corners[8] = {
        {localMin.x, localMin.y, localMin.z},
        {localMin.x, localMin.y, localMax.z},
        {localMin.x, localMax.y, localMin.z},
        {localMin.x, localMax.y, localMax.z},
        {localMax.x, localMin.y, localMin.z},
        {localMax.x, localMin.y, localMax.z},
        {localMax.x, localMax.y, localMin.z},
        {localMax.x, localMax.y, localMax.z}
    };

    outMin = glm::vec3(std::numeric_limits<float>::max());
    outMax = glm::vec3(-std::numeric_limits<float>::max());
    for (const auto& c : corners) {
        glm::vec3 wc = glm::vec3(modelMatrix * glm::vec4(c, 1.0f));
        outMin = glm::min(outMin, wc);
        outMax = glm::max(outMax, wc);
    }
}

// Closest point on triangle to a point (Ericson, Real-Time Collision Detection §5.1.5).
inline glm::vec3 closestPointOnTriangle(const glm::vec3& p,
                                         const glm::vec3& a, const glm::vec3& b, const glm::vec3& c) {
    glm::vec3 ab = b - a, ac = c - a, ap = p - a;
    float d1 = glm::dot(ab, ap), d2 = glm::dot(ac, ap);
    if (d1 <= 0.0f && d2 <= 0.0f) return a;
    glm::vec3 bp = p - b;
    float d3 = glm::dot(ab, bp), d4 = glm::dot(ac, bp);
    if (d3 >= 0.0f && d4 <= d3) return b;
    float vc = d1 * d4 - d3 * d2;
    if (vc <= 0.0f && d1 >= 0.0f && d3 <= 0.0f) {
        float v = d1 / (d1 - d3);
        return a + v * ab;
    }
    glm::vec3 cp = p - c;
    float d5 = glm::dot(ab, cp), d6 = glm::dot(ac, cp);
    if (d6 >= 0.0f && d5 <= d6) return c;
    float vb = d5 * d2 - d1 * d6;
    if (vb <= 0.0f && d2 >= 0.0f && d6 <= 0.0f) {
        float w = d2 / (d2 - d6);
        return a + w * ac;
    }
    float va = d3 * d6 - d5 * d4;
    if (va <= 0.0f && (d4 - d3) >= 0.0f && (d5 - d6) >= 0.0f) {
        float w = (d4 - d3) / ((d4 - d3) + (d5 - d6));
        return b + w * (c - b);
    }
    float denom = 1.0f / (va + vb + vc);
    float v = vb * denom;
    float w = vc * denom;
    return a + ab * v + ac * w;
}

// ---- Thread-local scratch buffers for collision queries ----
// Defined in m2_renderer_instance.cpp (inline thread_local causes LLD linker
// errors on Windows ARM64, so the definitions live in the single TU that uses them).

/// M2 billboard bone flags used by the 1.12 palette replacement.
/// 0x08 is spherical; 0x10/0x20/0x40 lock the authored X/Y/Z axis.
constexpr uint32_t kM2BoneBillboardSpherical = 0x08;
constexpr uint32_t kM2BoneBillboardLockX     = 0x10;
constexpr uint32_t kM2BoneBillboardLockY     = 0x20;
constexpr uint32_t kM2BoneBillboardLockZ     = 0x40;
constexpr uint32_t kM2BoneBillboardMask =
    kM2BoneBillboardSpherical | kM2BoneBillboardLockX |
    kM2BoneBillboardLockY | kM2BoneBillboardLockZ;

/// Resolve an M2 alias sequence to the sequence that owns its animation data.
///
/// Vanilla flag 0x40 marks an alias and aliasNext is an index into the same
/// sequence table. Keep the logical sequence index on the instance for its
/// duration/variation metadata, but sample every animation track from the
/// resolved target. Malformed/out-of-range chains and cycles fall back to the
/// logical entry instead of walking arbitrary memory or hanging the frame.
inline int resolveM2SequenceAlias(const M2ModelGPU& model, int sequenceIndex) {
    if (sequenceIndex < 0 ||
        sequenceIndex >= static_cast<int>(model.sequences.size())) {
        return sequenceIndex;
    }

    int resolved = sequenceIndex;
    for (size_t hop = 0; hop < model.sequences.size(); ++hop) {
        const auto& seq = model.sequences[resolved];
        if ((seq.flags & 0x40u) == 0) return resolved;

        const int next = static_cast<int>(seq.aliasNext);
        if (next < 0 || next >= static_cast<int>(model.sequences.size()) ||
            next == resolved) {
            return sequenceIndex;
        }
        resolved = next;
    }

    // More hops than there are sequence entries means the aliases cycle.
    return sequenceIndex;
}

inline void clearM2SequenceBlend(M2Instance& instance) {
    instance.blendFromSequenceIndex = -1;
    instance.blendFromAnimTime = 0.0f;
    instance.blendFromAnimSpeed = 1.0f;
    instance.blendElapsed = 0.0f;
    instance.blendDuration = 0.0f;
}

/// Arm a new logical sequence while preserving the outgoing clock for the
/// authored transition. The loader mirrors Vanilla M2Init first, so runtime
/// flag 0x80 is the single authoritative "blend this sequence" bit here.
inline void beginM2SequenceTransition(M2Instance& instance,
                                      const M2ModelGPU& model,
                                      int nextSequenceIndex,
                                      float nextAnimTime = 0.0f,
                                      bool allowBlend = true) {
    if (nextSequenceIndex < 0 ||
        nextSequenceIndex >= static_cast<int>(model.sequences.size())) {
        return;
    }

    const int previousIndex = instance.currentSequenceIndex;
    const auto& next = model.sequences[nextSequenceIndex];
    const bool blendFlag = (next.flags & 0x80u) != 0;
    const float authoredBlend =
        blendFlag ? static_cast<float>(next.blendTime) : 0.0f;

    if (allowBlend && authoredBlend > 0.0f &&
        previousIndex >= 0 &&
        previousIndex < static_cast<int>(model.sequences.size()) &&
        previousIndex != nextSequenceIndex) {
        instance.blendFromSequenceIndex = previousIndex;
        instance.blendFromAnimTime = instance.animTime;
        instance.blendFromAnimSpeed = instance.animSpeed;
        instance.blendElapsed = 0.0f;
        instance.blendDuration = authoredBlend;
    } else {
        clearM2SequenceBlend(instance);
    }

    instance.currentSequenceIndex = nextSequenceIndex;
    instance.animDuration = static_cast<float>(next.duration);
    instance.animTime = nextAnimTime;
}

/// 1.12.1 sequence cross-fade weight: smoothstep, not a linear ramp.
inline float m2SequenceBlendWeight(const M2Instance& instance) {
    if (instance.blendFromSequenceIndex < 0 ||
        instance.blendDuration <= 0.0f) {
        return 1.0f;
    }
    const float t = glm::clamp(
        instance.blendElapsed / instance.blendDuration, 0.0f, 1.0f);
    return (3.0f - 2.0f * t) * t * t;
}

/// Bone transforms for one instance.
///
/// `cameraBasisWorld` columns are right/up/forward. Vanilla 1.12 rewrites a
/// billboard bone from this shared VIEW basis after its ordinary authored
/// parent/TRS composition; it does not aim each bone from its pivot at the
/// camera position.
inline void computeBoneMatrices(const M2ModelGPU& model, M2Instance& instance,
                                const glm::mat3* cameraBasisWorld = nullptr) {
    ZoneScopedN("M2::computeBoneMatrices");
    size_t numBones = std::min(model.bones.size(), size_t(kMaxBonesPerInstance));
    if (numBones == 0) return;
    instance.boneMatrices.resize(numBones);
    const auto& gsd = model.globalSequenceDurations;
    const int sampleSequenceIndex =
        resolveM2SequenceAlias(model, instance.currentSequenceIndex);
    const bool sequenceBlending =
        instance.blendFromSequenceIndex >= 0 &&
        instance.blendDuration > 0.0f;
    const int blendSequenceIndex = sequenceBlending
        ? resolveM2SequenceAlias(model, instance.blendFromSequenceIndex)
        : -1;
    const float blendWeight = sequenceBlending
        ? m2SequenceBlendWeight(instance) : 1.0f;

    for (size_t i = 0; i < numBones; i++) {
        const auto& bone = model.bones[i];
        glm::vec3 trans = m2_track::sampleVec3(
            bone.translation, sampleSequenceIndex, instance.animTime,
            instance.globalSequenceTime, gsd, glm::vec3(0.0f));
        glm::quat rot = m2_track::sampleQuat(
            bone.rotation, sampleSequenceIndex, instance.animTime,
            instance.globalSequenceTime, gsd);
        glm::vec3 scl = m2_track::sampleVec3(
            bone.scale, sampleSequenceIndex, instance.animTime,
            instance.globalSequenceTime, gsd, glm::vec3(1.0f));

        // The Classic evaluator samples the outgoing and incoming sequence
        // clocks independently, then cross-fades continuous TRS values. Global
        // sequence tracks already ignore the local sequence/time, so sampling
        // both legs yields the same value and remains phase-locked.
        if (sequenceBlending && blendSequenceIndex >= 0) {
            const glm::vec3 fromTrans = m2_track::sampleVec3(
                bone.translation, blendSequenceIndex, instance.blendFromAnimTime,
                instance.globalSequenceTime, gsd, glm::vec3(0.0f));
            const glm::quat fromRot = m2_track::sampleQuat(
                bone.rotation, blendSequenceIndex, instance.blendFromAnimTime,
                instance.globalSequenceTime, gsd);
            const glm::vec3 fromScale = m2_track::sampleVec3(
                bone.scale, blendSequenceIndex, instance.blendFromAnimTime,
                instance.globalSequenceTime, gsd, glm::vec3(1.0f));

            trans = glm::mix(fromTrans, trans, blendWeight);
            rot = glm::normalize(glm::slerp(fromRot, rot, blendWeight));
            scl = glm::mix(fromScale, scl, blendWeight);
        }

        if (scl.x < 0.001f) scl.x = 1.0f;
        if (scl.y < 0.001f) scl.y = 1.0f;
        if (scl.z < 0.001f) scl.z = 1.0f;

        glm::mat4 local = glm::translate(glm::mat4(1.0f), bone.pivot);
        local = glm::translate(local, trans);
        local *= glm::toMat4(rot);
        local = glm::scale(local, scl);
        local = glm::translate(local, -bone.pivot);

        glm::mat4 composed = local;
        if (bone.parentBone >= 0 && static_cast<size_t>(bone.parentBone) < numBones) {
            const glm::mat4 parent = instance.boneMatrices[bone.parentBone];
            glm::mat4 effectiveParent = parent;

            // Vanilla bone flags 0x01/0x02/0x04 rewrite the effective parent
            // before this bone's own TRS composes:
            //   0x01 ignore parent translation
            //   0x02 ignore parent scale
            //   0x04 ignore parent rotation
            // Root/model placement lives outside our palette in modelMatrix,
            // so the model-root basis here is identity.
            const uint32_t parentArm = bone.flags & 0x07u;
            if (parentArm != 0) {
                const glm::vec3 p0(parent[0]);
                const glm::vec3 p1(parent[1]);
                const glm::vec3 p2(parent[2]);
                const glm::vec3 plen(
                    glm::length(p0), glm::length(p1), glm::length(p2));

                glm::mat3 basis(1.0f);
                switch (parentArm & 0x06u) {
                    case 0x00u:
                        basis[0] = p0;
                        basis[1] = p1;
                        basis[2] = p2;
                        break;
                    case 0x02u: { // ignore parent scale
                        auto unitOr = [](const glm::vec3& v, const glm::vec3& fallback) {
                            const float l2 = glm::dot(v, v);
                            return l2 > (1.0f / float(1 << 22))
                                ? v * glm::inversesqrt(l2) : fallback;
                        };
                        basis[0] = unitOr(p0, glm::vec3(1, 0, 0));
                        basis[1] = unitOr(p1, glm::vec3(0, 1, 0));
                        basis[2] = unitOr(p2, glm::vec3(0, 0, 1));
                        break;
                    }
                    case 0x04u: // ignore parent rotation; keep per-axis scale
                        basis[0] = glm::vec3(plen.x, 0, 0);
                        basis[1] = glm::vec3(0, plen.y, 0);
                        basis[2] = glm::vec3(0, 0, plen.z);
                        break;
                    case 0x06u: // ignore parent rotation and scale
                        basis = glm::mat3(1.0f);
                        break;
                }

                glm::vec3 translation(0.0f);
                if ((parentArm & 0x01u) == 0) {
                    // Preserve the animated parent's placement of this bone's
                    // bind pivot while replacing only the requested basis parts.
                    const glm::vec3 posedPivot =
                        glm::vec3(parent * glm::vec4(bone.pivot, 1.0f));
                    translation = posedPivot - basis * bone.pivot;
                }

                effectiveParent = glm::mat4(1.0f);
                effectiveParent[0] = glm::vec4(basis[0], 0.0f);
                effectiveParent[1] = glm::vec4(basis[1], 0.0f);
                effectiveParent[2] = glm::vec4(basis[2], 0.0f);
                effectiveParent[3] = glm::vec4(translation, 1.0f);
            }

            composed = effectiveParent * local;
        }

        if (cameraBasisWorld && (bone.flags & kM2BoneBillboardMask) != 0) {
            // Convert the camera's shared world-space view basis into this
            // instance's model space. Direction vectors use w=0 so placement
            // translation cannot leak into billboard orientation.
            const glm::mat3 invModel(instance.invModelMatrix);
            auto modelDir = [&](const glm::vec3& worldDir, const glm::vec3& fallback) {
                glm::vec3 v = invModel * worldDir;
                const float l2 = glm::dot(v, v);
                return l2 > 1e-10f ? v * glm::inversesqrt(l2) : fallback;
            };
            const glm::vec3 camRight = modelDir((*cameraBasisWorld)[0], glm::vec3(0, -1, 0));
            const glm::vec3 camUp    = modelDir((*cameraBasisWorld)[1], glm::vec3(0, 0, 1));
            const glm::vec3 camFwd   = modelDir((*cameraBasisWorld)[2], glm::vec3(-1, 0, 0));

            // Preserve the ordinary composed pivot and per-axis scale. The
            // reference replaces the palette basis, not the bone's position or
            // authored scale (Lightwell's non-uniform lock-Z scale depends on it).
            const glm::vec3 posedPivot = glm::vec3(composed * glm::vec4(bone.pivot, 1.0f));
            const glm::vec3 preX(composed[0]);
            const glm::vec3 preY(composed[1]);
            const glm::vec3 preZ(composed[2]);
            const glm::vec3 axisScale(
                std::max(glm::length(preX), 1e-6f),
                std::max(glm::length(preY), 1e-6f),
                std::max(glm::length(preZ), 1e-6f));

            auto unitOr = [](const glm::vec3& v, const glm::vec3& fallback) {
                const float l2 = glm::dot(v, v);
                return l2 > 1e-10f ? v * glm::inversesqrt(l2) : fallback;
            };

            glm::vec3 bx, by, bz;
            if ((bone.flags & kM2BoneBillboardSpherical) != 0) {
                // X toward viewer, Y screen-right, Z screen-up.
                bx = -camFwd;
                by = camRight;
                bz = camUp;
            } else if ((bone.flags & kM2BoneBillboardLockX) != 0) {
                bx = unitOr(preX, -camFwd);
                bz = unitOr(glm::cross(camFwd, bx), camUp);
                by = unitOr(glm::cross(bz, bx), camRight);
            } else if ((bone.flags & kM2BoneBillboardLockY) != 0) {
                by = unitOr(preY, camRight);
                bx = unitOr(glm::cross(camFwd, by), -camFwd);
                bz = unitOr(glm::cross(bx, by), camUp);
            } else {
                // Lock-Z (0x40): keep authored model up and spin in-plane.
                bz = unitOr(preZ, camUp);
                by = unitOr(glm::cross(camFwd, bz), camRight);
                bx = unitOr(glm::cross(by, bz), -camFwd);
            }

            glm::mat4 basis(1.0f);
            basis[0] = glm::vec4(bx * axisScale.x, 0.0f);
            basis[1] = glm::vec4(by * axisScale.y, 0.0f);
            basis[2] = glm::vec4(bz * axisScale.z, 0.0f);
            instance.boneMatrices[i] =
                glm::translate(glm::mat4(1.0f), posedPivot) *
                basis *
                glm::translate(glm::mat4(1.0f), -bone.pivot);
        } else {
            instance.boneMatrices[i] = composed;
        }
    }
    instance.bonesDirty[0] = instance.bonesDirty[1] = true;
}

// Firelight guttering for lamps, torches and braziers.
//
// The phase is hashed from the fixture's world position so no two lights ever
// pulse together - a synchronised row of street lamps reads as a rendering
// artifact rather than firelight - and it stays stable frame to frame because
// it derives from a fixed position rather than a counter.
//
// Two detuned sines with periods of roughly 3.7 and 1.6 seconds, sharing no
// common multiple over any watchable span. Slower than this and the eye adapts
// to the change and stops seeing it at all - a 15-second swell is invisible no
// matter how deep it goes - while faster reads as a strobe.
//
// phaseSeed must be a position that does not move frame to frame - the fixture's
// placement, never an animated bone centre or a sprite offset toward the camera.
// The hash is chaotic by design, so a seed that drifts even slightly re-rolls the
// phase every frame and the result is a strobe rather than a flicker. It is also
// quantised to a one-unit grid here as insurance against a caller passing
// something that creeps.
inline float lampFlicker(const glm::vec3& phaseSeed, float seconds,
                         float base, float slowAmp, float fastAmp) {
    const glm::vec3 cell = glm::floor(phaseSeed);
    float h = std::sin(cell.x * 12.9898f + cell.y * 78.233f +
                       cell.z * 37.719f) * 43758.5453f;
    const float phase = (h - std::floor(h)) * 6.2831853f;
    return base + slowAmp * std::sin(seconds * 1.70f + phase)
                + fastAmp * std::sin(seconds * 3.90f + phase * 1.7f);
}

/// Seconds since process start, shared by the flicker animations.
inline float lampFlickerClockSeconds() {
    static const auto start = std::chrono::steady_clock::now();
    return std::chrono::duration<float>(std::chrono::steady_clock::now() - start).count();
}

} // namespace m2_internal

// Pull all symbols into the rendering namespace so existing code compiles unchanged
using namespace m2_internal;

} // namespace rendering
} // namespace wowee

