// Exact WoW 1.12 MDDF/MODF placement contract.
#include <catch_amalgamated.hpp>

#include <cmath>
#include <glm/glm.hpp>

#include "rendering/placement_transform.hpp"

using wowee::rendering::placementEulerFromAdtDegrees;
using wowee::rendering::placementModelMatrix;

namespace {
glm::mat3 rotX(float a) {
    const float c=std::cos(a), s=std::sin(a);
    return glm::mat3(1,0,0, 0,c,s, 0,-s,c);
}
glm::mat3 rotY(float a) {
    const float c=std::cos(a), s=std::sin(a);
    return glm::mat3(c,0,-s, 0,1,0, s,0,c);
}
glm::mat3 rotZ(float a) {
    const float c=std::cos(a), s=std::sin(a);
    return glm::mat3(c,s,0, -s,c,0, 0,0,1);
}
}

TEST_CASE("Vanilla ADT rotation fields map to X/Y/Z exactly", "[placement][wow112]") {
    float raw[3] = {10.0f, 20.0f, 30.0f}; // +14 Y, +18 Z, +1c X
    const glm::vec3 e = placementEulerFromAdtDegrees(raw);
    constexpr float d = wowee::core::coords::PI / 180.0f;
    CHECK(e.x == Catch::Approx(30.0f*d));
    CHECK(e.y == Catch::Approx(10.0f*d));
    CHECK(e.z == Catch::Approx(200.0f*d));
}

TEST_CASE("Vanilla placement composes Rz Ry Rx", "[placement][wow112]") {
    const glm::vec3 e(0.30f,-0.70f,1.10f);
    const glm::mat3 expected = rotZ(e.z) * rotY(e.y) * rotX(e.x);
    const glm::mat3 got(placementModelMatrix({0,0,0},e,1.0f));
    for(int c=0;c<3;++c)
      for(int r=0;r<3;++r)
        CHECK(got[c][r] == Catch::Approx(expected[c][r]).margin(1e-5));
}

TEST_CASE("translation is not scaled", "[placement][wow112]") {
    const glm::mat4 m=placementModelMatrix({7,-2,5},{0.2f,0.3f,0.4f},3.0f);
    CHECK(m[3][0] == Catch::Approx(7.0f));
    CHECK(m[3][1] == Catch::Approx(-2.0f));
    CHECK(m[3][2] == Catch::Approx(5.0f));
}
