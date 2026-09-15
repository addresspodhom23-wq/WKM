#include <cassert>
#include <cmath>
#include <cstdint>

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

    return 0;
}
