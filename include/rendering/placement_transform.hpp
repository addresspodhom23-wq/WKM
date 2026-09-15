#pragma once

// Vanilla WoW 1.12 MDDF/MODF placement transform.
//
// The 1.12.1 client builds a column-major model matrix as:
//   M = T(worldPos) * Rz(aZ) * Ry(aY) * Rx(aX) * S
// where the on-disk rotation fields map as:
//   +0x14 -> Y
//   +0x18 -> Z, with +180 degrees
//   +0x1c -> X
//
// MDDF and MODF share this convention. Keeping the raw-field mapping and
// composition here prevents WMO and M2 placement from drifting apart.

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "core/coordinates.hpp"

namespace wowee::rendering {

inline glm::vec3 placementEulerFromAdtDegrees(const float rotation[3]) {
    constexpr float kDeg = core::coords::PI / 180.0f;
    return glm::vec3(
        rotation[2] * kDeg,                  // aX = +0x1c
        rotation[0] * kDeg,                  // aY = +0x14
        (rotation[1] + 180.0f) * kDeg);      // aZ = +0x18 + pi
}

/// Translate, then compose the active rotations exactly as the 1.12 client:
/// Rz * Ry * Rx. GLM post-multiplies, so call them in Z, Y, X order.
inline glm::mat4 placementModelMatrix(const glm::vec3& position,
                                      const glm::vec3& eulerRadians,
                                      float scale) {
    glm::mat4 m(1.0f);
    m = glm::translate(m, position);
    m = glm::rotate(m, eulerRadians.z, glm::vec3(0.0f, 0.0f, 1.0f));
    m = glm::rotate(m, eulerRadians.y, glm::vec3(0.0f, 1.0f, 0.0f));
    m = glm::rotate(m, eulerRadians.x, glm::vec3(1.0f, 0.0f, 0.0f));
    m = glm::scale(m, glm::vec3(scale));
    return m;
}

}  // namespace wowee::rendering
