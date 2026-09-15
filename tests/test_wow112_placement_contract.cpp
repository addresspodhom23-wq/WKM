#include <cassert>
#include <cmath>
#include <glm/glm.hpp>

#include "rendering/placement_transform.hpp"

using wowee::rendering::placementEulerFromAdtDegrees;
using wowee::rendering::placementModelMatrix;

static glm::mat3 rx(float a){float c=std::cos(a),s=std::sin(a);return {1,0,0,0,c,s,0,-s,c};}
static glm::mat3 ry(float a){float c=std::cos(a),s=std::sin(a);return {c,0,-s,0,1,0,s,0,c};}
static glm::mat3 rz(float a){float c=std::cos(a),s=std::sin(a);return {c,s,0,-s,c,0,0,0,1};}

int main(){
    float raw[3]={10.0f,20.0f,30.0f};
    const auto e=placementEulerFromAdtDegrees(raw);
    constexpr float d=wowee::core::coords::PI/180.0f;
    assert(std::abs(e.x-30.0f*d)<1e-5f);
    assert(std::abs(e.y-10.0f*d)<1e-5f);
    assert(std::abs(e.z-200.0f*d)<1e-5f);

    const glm::vec3 a(0.3f,-0.7f,1.1f);
    const glm::mat3 got(placementModelMatrix({0,0,0},a,1.0f));
    const glm::mat3 want=rz(a.z)*ry(a.y)*rx(a.x);
    for(int c=0;c<3;++c) for(int r=0;r<3;++r)
        assert(std::abs(got[c][r]-want[c][r])<1e-5f);
    return 0;
}
