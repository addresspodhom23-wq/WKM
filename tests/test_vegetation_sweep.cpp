#include "rendering/capsule_sweep.hpp"
#include <cassert>
#include <iostream>

// Keep this regression executable independent of renderer/Vulkan/GLM setup.
struct Vec {
    float x,y,z;
    Vec(float a,float b,float c):x(a),y(b),z(c){}
    Vec operator+(Vec b) const {return {x+b.x,y+b.y,z+b.z};}
    Vec operator-(Vec b) const {return {x-b.x,y-b.y,z-b.z};}
    Vec operator*(float s) const {return {x*s,y*s,z*s};}
};
int main() {
    using wowee::rendering::capsule_sweep::safeFraction;
    const Vec a(0,-10,-10),b(0,10,-10),c(0,0,10);
    auto sweep=[&](float x,Vec motion) {
        return safeFraction(Vec(x,0,.4f),Vec(x,0,1.6f),motion,.4f,a,b,c);
    };
    const float hit=sweep(-2,Vec(4,0,0));
    assert(hit>.39f && hit<.41f); // Both endpoints clear; crosses a thin face.
    assert(sweep(2,Vec(-4,0,0))>.39f && sweep(2,Vec(-4,0,0))<.41f);
    assert(sweep(-.5f,Vec(0,2,0))==1); // Tangential motion.
    assert(sweep(-.2f,Vec(-1,0,0))==1); // Escape initial penetration.
    assert(sweep(-.2f,Vec(1,0,0))==0); // Do not deepen penetration.
    assert(sweep(-2,Vec(0,0,0))==1);
    assert(sweep(-2,Vec(100,0,0))<.017f); // Large frame step.
    assert(safeFraction(Vec(-2,0,.4f),Vec(-2,0,1.6f),Vec(4,0,0),
                        .4f,c,b,a)>.39f); // Reversed winding.
    assert(safeFraction(Vec(-2,0,20),Vec(-2,0,21),Vec(4,0,0),
                        .4f,a,b,c)==1); // Above the tree.
    // A small face touches the middle of the capsule, not either endpoint.
    assert(safeFraction(Vec(-2,0,.4f),Vec(-2,0,1.6f),Vec(4,0,0),.4f,
                        Vec(0,-.1f,.9f),Vec(0,.1f,.9f),Vec(0,0,1.1f))<.5f);
    // Clear beside a narrow trunk; no canopy-sized box.
    assert(safeFraction(Vec(-2,2,.4f),Vec(-2,2,1.6f),Vec(4,0,0),.4f,
                        Vec(0,-.1f,0),Vec(0,.1f,0),Vec(0,0,3))==1);
    // Degenerate geometry must not produce NaNs.
    const float point=safeFraction(Vec(-2,0,.4f),Vec(-2,0,1.6f),Vec(4,0,0),
                                   .4f,Vec(0,0,1),Vec(0,0,1),Vec(0,0,1));
    assert(std::isfinite(point) && point<.5f);
    // Translation and uniform scaling leave the collision fraction unchanged.
    const Vec offset(12,-4,7);
    const float scaled=safeFraction(Vec(-2,0,.4f)*3+offset,
        Vec(-2,0,1.6f)*3+offset,Vec(12,0,0),1.2f,a*3+offset,b*3+offset,c*3+offset);
    assert(std::abs(scaled-hit)<.0001f);
    std::cout << "Vegetation capsule sweep regressions passed\n";
}
