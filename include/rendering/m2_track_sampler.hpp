#pragma once

#include "pipeline/m2_loader.hpp"

#include <glm/common.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

namespace wowee::rendering::m2_track {

struct SampleTime {
    int sequenceIndex = -1;
    float timeMs = 0.0f;
};

inline SampleTime resolveTime(const pipeline::M2AnimationTrack& track,
                              int animationSequenceIndex, float animationTimeMs,
                              float globalTimeMs,
                              const std::vector<uint32_t>& globalSequenceDurations) {
    if (track.globalSequence >= 0 &&
        static_cast<size_t>(track.globalSequence) < globalSequenceDurations.size()) {
        const float duration =
            static_cast<float>(globalSequenceDurations[track.globalSequence]);
        float time = duration > 0.0f ? std::fmod(globalTimeMs, duration) : 0.0f;
        if (time < 0.0f) time += duration;
        return {.sequenceIndex = 0, .timeMs = time};
    }
    return {.sequenceIndex = animationSequenceIndex, .timeMs = animationTimeMs};
}

inline size_t lowerKeyIndex(const std::vector<uint32_t>& timestamps,
                            size_t keyCount, float timeMs) {
    keyCount = std::min(keyCount, timestamps.size());
    if (keyCount <= 1 || timeMs <= static_cast<float>(timestamps[0])) return 0;
    const auto end = timestamps.begin() + static_cast<std::ptrdiff_t>(keyCount);
    const auto upper = std::upper_bound(
        timestamps.begin(), end, timeMs,
        [](float time, uint32_t timestamp) {
            return time < static_cast<float>(timestamp);
        });
    if (upper == end) return keyCount - 1;
    return static_cast<size_t>(upper - timestamps.begin() - 1);
}

inline float interpolationFraction(const pipeline::M2AnimationTrack& track,
                                   const std::vector<uint32_t>& timestamps,
                                   size_t lower, size_t keyCount, float timeMs) {
    if (track.interpolationType == 0 || lower + 1 >= keyCount) return 0.0f;
    const float t0 = static_cast<float>(timestamps[lower]);
    const float t1 = static_cast<float>(timestamps[lower + 1]);
    return t1 > t0 ? glm::clamp((timeMs - t0) / (t1 - t0), 0.0f, 1.0f) : 0.0f;
}

// Vanilla/Classic M2 uses the original Blizzard ordering:
//   0=None, 1=Linear, 2=Bezier, 3=Hermite/spline.
// Spline records store {value, inTan, outTan} per key. Between key i and i+1
// the segment leaves through i.outTan and arrives through (i+1).inTan.
template<typename T>
inline T interpolateHermite(float t, const T& p0, const T& p1,
                            const T& out0, const T& in1) {
    const float t2 = t * t;
    const float t3 = t2 * t;
    const float h00 = 2.0f * t3 - 3.0f * t2 + 1.0f;
    const float h10 = t3 - 2.0f * t2 + t;
    const float h01 = -2.0f * t3 + 3.0f * t2;
    const float h11 = t3 - t2;
    return p0 * h00 + out0 * h10 + p1 * h01 + in1 * h11;
}

template<typename T>
inline T interpolateBezier(float t, const T& p0, const T& p1,
                           const T& out0, const T& in1) {
    const float u = 1.0f - t;
    const float u2 = u * u;
    const float t2 = t * t;
    return p0 * (u2 * u) +
           out0 * (3.0f * u2 * t) +
           in1 * (3.0f * u * t2) +
           p1 * (t2 * t);
}

template<typename T>
inline T interpolateAuthored(uint16_t type, float fraction,
                             const T& p0, const T& p1,
                             const T& out0, const T& in1) {
    if (type == 2) return interpolateBezier(fraction, p0, p1, out0, in1);
    if (type == 3) return interpolateHermite(fraction, p0, p1, out0, in1);
    return p0 * (1.0f - fraction) + p1 * fraction;
}

inline float sampleFloat(const pipeline::M2AnimationTrack& track,
                         int animationSequenceIndex, float animationTimeMs,
                         float globalTimeMs,
                         const std::vector<uint32_t>& globalSequenceDurations,
                         float defaultValue) {
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    if (sampleTime.sequenceIndex < 0 ||
        static_cast<size_t>(sampleTime.sequenceIndex) >= track.sequences.size()) {
        return defaultValue;
    }
    const auto& keys = track.sequences[static_cast<size_t>(sampleTime.sequenceIndex)];
    const size_t count = std::min(keys.timestamps.size(), keys.floatValues.size());
    if (count == 0) return defaultValue;
    const size_t lower = lowerKeyIndex(keys.timestamps, count, sampleTime.timeMs);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    if (lower + 1 >= count) return keys.floatValues[lower];

    if ((track.interpolationType == 2 || track.interpolationType == 3) &&
        keys.floatInTangents.size() >= count &&
        keys.floatOutTangents.size() >= count) {
        return interpolateAuthored(track.interpolationType, fraction,
                                   keys.floatValues[lower], keys.floatValues[lower + 1],
                                   keys.floatOutTangents[lower],
                                   keys.floatInTangents[lower + 1]);
    }
    return glm::mix(keys.floatValues[lower], keys.floatValues[lower + 1], fraction);
}

inline glm::vec3 sampleVec3(const pipeline::M2AnimationTrack& track,
                            int animationSequenceIndex, float animationTimeMs,
                            float globalTimeMs,
                            const std::vector<uint32_t>& globalSequenceDurations,
                            const glm::vec3& defaultValue) {
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    if (sampleTime.sequenceIndex < 0 ||
        static_cast<size_t>(sampleTime.sequenceIndex) >= track.sequences.size()) {
        return defaultValue;
    }
    const auto& keys = track.sequences[static_cast<size_t>(sampleTime.sequenceIndex)];
    const size_t count = std::min(keys.timestamps.size(), keys.vec3Values.size());
    if (count == 0) return defaultValue;
    const auto safe = [&](const glm::vec3& value) {
        return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z)
            ? value : defaultValue;
    };
    const size_t lower = lowerKeyIndex(keys.timestamps, count, sampleTime.timeMs);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    if (lower + 1 >= count) return safe(keys.vec3Values[lower]);

    glm::vec3 value;
    if ((track.interpolationType == 2 || track.interpolationType == 3) &&
        keys.vec3InTangents.size() >= count &&
        keys.vec3OutTangents.size() >= count) {
        value = interpolateAuthored(track.interpolationType, fraction,
                                    safe(keys.vec3Values[lower]),
                                    safe(keys.vec3Values[lower + 1]),
                                    safe(keys.vec3OutTangents[lower]),
                                    safe(keys.vec3InTangents[lower + 1]));
    } else {
        value = glm::mix(safe(keys.vec3Values[lower]),
                         safe(keys.vec3Values[lower + 1]), fraction);
    }
    return safe(value);
}

inline glm::quat sampleQuat(const pipeline::M2AnimationTrack& track,
                            int animationSequenceIndex, float animationTimeMs,
                            float globalTimeMs,
                            const std::vector<uint32_t>& globalSequenceDurations) {
    const glm::quat identity(1.0f, 0.0f, 0.0f, 0.0f);
    const auto sampleTime = resolveTime(track, animationSequenceIndex,
                                        animationTimeMs, globalTimeMs,
                                        globalSequenceDurations);
    if (sampleTime.sequenceIndex < 0 ||
        static_cast<size_t>(sampleTime.sequenceIndex) >= track.sequences.size()) {
        return identity;
    }
    const auto& keys = track.sequences[static_cast<size_t>(sampleTime.sequenceIndex)];
    const size_t count = std::min(keys.timestamps.size(), keys.quatValues.size());
    if (count == 0) return identity;
    const auto finiteQuat = [](const glm::quat& value) {
        return std::isfinite(value.w) && std::isfinite(value.x) &&
               std::isfinite(value.y) && std::isfinite(value.z);
    };
    const auto safe = [&](const glm::quat& value) {
        const float lengthSquared = glm::dot(value, value);
        return finiteQuat(value) && std::isfinite(lengthSquared) &&
               lengthSquared >= 0.000001f
            ? glm::normalize(value) : identity;
    };
    const auto control = [&](const glm::quat& value) {
        return finiteQuat(value) ? value : glm::quat(0.0f, 0.0f, 0.0f, 0.0f);
    };
    const size_t lower = lowerKeyIndex(keys.timestamps, count, sampleTime.timeMs);
    const float fraction = interpolationFraction(track, keys.timestamps, lower,
                                                 count, sampleTime.timeMs);
    if (lower + 1 >= count) return safe(keys.quatValues[lower]);

    if ((track.interpolationType == 2 || track.interpolationType == 3) &&
        keys.quatInTangents.size() >= count &&
        keys.quatOutTangents.size() >= count) {
        // Blizzard stores quaternion spline controls in the same four-component
        // key shape. Evaluate the authored cubic component-wise, then restore
        // the unit-quaternion invariant before the matrix conversion.
        return safe(interpolateAuthored(track.interpolationType, fraction,
                                        safe(keys.quatValues[lower]),
                                        safe(keys.quatValues[lower + 1]),
                                        control(keys.quatOutTangents[lower]),
                                        control(keys.quatInTangents[lower + 1])));
    }
    return safe(glm::slerp(safe(keys.quatValues[lower]),
                           safe(keys.quatValues[lower + 1]), fraction));
}

} // namespace wowee::rendering::m2_track
