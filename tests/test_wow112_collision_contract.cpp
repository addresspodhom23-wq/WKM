#include <cassert>
#include <cmath>
#include <cstdint>
#include <fstream>
#include <sstream>
#include <string>

#include "rendering/collision_geometry.hpp"

using wowee::rendering::crossesCollisionPlane;
using wowee::rendering::wmoMopyCollidable;

int main() {
    // Vanilla MOPY contract.
    assert(wmoMopyCollidable(0x08, 0x00));       // explicit collision hull
    assert(wmoMopyCollidable(0x20, 0x00));       // rendered solid
    assert(!wmoMopyCollidable(0x24, 0x00));      // rendered detail is decorative
    assert(!wmoMopyCollidable(0x04, 0x00));      // detail-only
    assert(wmoMopyCollidable(0x00, 0xFF));       // collision-only material
    assert(!wmoMopyCollidable(0x00, 0x00));

    // The regression from the Tirisfal house: a normal frame step can start
    // already inside the player's collision radius. Crossing the plane still
    // MUST be detected.
    assert(crossesCollisionPlane(+0.20f, -0.15f));
    assert(crossesCollisionPlane(-0.20f, +0.15f));
    assert(!crossesCollisionPlane(+0.20f, +0.05f));
    assert(!crossesCollisionPlane(-0.20f, -0.05f));

    // Movement keeps a one-shot geometry guard after the normal cylinder
    // solver. It must only run when the solver saw no WMO contact, otherwise
    // it would destroy the original wall-sliding response.
    std::ifstream movement("src/rendering/camera_controller.cpp");
    assert(movement.good());
    std::ostringstream movementText;
    movementText << movement.rdbuf();
    const std::string source = movementText.str();
    assert(source.find("bool wmoAdjusted = false") != std::string::npos);
    assert(source.find("wmoRenderer && !wmoAdjusted") != std::string::npos);
    assert(source.find("segmentBlocked(guardStart, guardEnd)") != std::string::npos);
    assert(source.find("kEndpointExtension = 0.55f") != std::string::npos);

    return 0;
}
