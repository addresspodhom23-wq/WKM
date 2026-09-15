"""Compile the production bounds policy without its Vulkan renderer dependencies."""
from pathlib import Path
import os
import subprocess
import tempfile

root = Path(__file__).resolve().parents[1]
source = (root / "src/rendering/m2_renderer_internal.h").read_text()
start = source.index("inline void getTightCollisionBounds(")
end = source.index("\ninline float getEffectiveCollisionTopLocal", start)
policy = source[start:end]

# Only the vector arithmetic and model fields used by this pure policy are
# supplied here. The function under test is taken verbatim from the renderer.
harness = r"""
#include <algorithm>
#include <cassert>
namespace glm {
struct vec3 {
    float x=0,y=0,z=0;
    vec3()=default;
    vec3(float a,float b,float c):x(a),y(b),z(c){}
    vec3 operator+(vec3 b) const {return {x+b.x,y+b.y,z+b.z};}
    vec3 operator-(vec3 b) const {return {x-b.x,y-b.y,z-b.z};}
    vec3 operator*(float s) const {return {x*s,y*s,z*s};}
};
}
struct M2ModelGPU {
    glm::vec3 boundMin{-20,-20,-5},boundMax{40,40,40};
    bool isFoliageLike=true,collisionTreeTrunk=true;
    bool collisionNarrowVerticalProp=false,collisionSmallSolidProp=false;
    bool collisionSteppedLowPlatform=false;
    struct {
        bool present=true;
        glm::vec3 boundsMin{-12,-3,-2},boundsMax{-8,3,25};
        bool valid() const {return present;}
    } collision;
};
""" + policy + r"""
int main() {
    M2ModelGPU tree;
    glm::vec3 lo,hi;
    getTightCollisionBounds(tree,lo,hi);
    // Offset trunk, outside the old box around the visual canopy centre.
    assert(lo.x<=-12 && hi.x>=-8 && lo.y<=-3 && hi.y>=3);
    // Upper trunk cannot disappear from broad-phase collision.
    assert(lo.z<=-2 && hi.z>=25);
    tree.collisionTreeTrunk=false; // Narrow trees also use authored bounds.
    getTightCollisionBounds(tree,lo,hi);
    assert(lo.x==-12 && hi.z==25);
    tree.collision.present=false; // No authored mesh: retain existing fallback.
    getTightCollisionBounds(tree,lo,hi);
    assert(lo.x!=-12);
}
"""
with tempfile.TemporaryDirectory() as temp:
    cpp = Path(temp) / "bounds.cpp"
    exe = Path(temp) / "bounds"
    cpp.write_text(harness)
    subprocess.run([os.environ.get("CXX", "g++"), "-std=c++20", "-Wall",
                    "-Wextra", "-Werror", str(cpp), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
print("Authored vegetation broad-phase bounds regressions passed")
