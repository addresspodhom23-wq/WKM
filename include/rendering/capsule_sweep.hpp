#pragma once
#include <algorithm>
#include <cmath>
#include <initializer_list>

namespace wowee::rendering::capsule_sweep {

// Vector is a three-component vector with arithmetic (glm::vec3 in the client).
template<class V> float scalarProduct(const V& a, const V& b) {
    return a.x*b.x + a.y*b.y + a.z*b.z;
}
template<class V> V vectorProduct(const V& a, const V& b) {
    return V(a.y*b.z-a.z*b.y, a.z*b.x-a.x*b.z, a.x*b.y-a.y*b.x);
}
template<class V> V segmentPoint(const V& p, const V& a, const V& b) {
    const V e = b-a;
    const float n = scalarProduct(e,e);
    return a + e * (n > 1e-12f ? std::clamp(scalarProduct(p-a,e)/n,0.0f,1.0f) : 0.0f);
}
template<class V> V trianglePoint(const V& p, const V& a, const V& b, const V& c) {
    const V ab=b-a, ac=c-a, ap=p-a;
    const V normal=vectorProduct(ab,ac);
    if (scalarProduct(normal,normal) < 1e-12f) {
        V best=segmentPoint(p,a,b);
        for (const V q : {segmentPoint(p,b,c),segmentPoint(p,c,a)})
            if (scalarProduct(p-q,p-q)<scalarProduct(p-best,p-best)) best=q;
        return best;
    }
    const float d1=scalarProduct(ab,ap), d2=scalarProduct(ac,ap);
    if(d1<=0 && d2<=0) return a;
    const V bp=p-b;
    const float d3=scalarProduct(ab,bp),d4=scalarProduct(ac,bp);
    if(d3>=0 && d4<=d3) return b;
    const float vc=d1*d4-d3*d2;
    if(vc<=0 && d1>=0 && d3<=0) return a+ab*(d1/(d1-d3));
    const V cp=p-c;
    const float d5=scalarProduct(ab,cp),d6=scalarProduct(ac,cp);
    if(d6>=0 && d5<=d6) return c;
    const float vb=d5*d2-d1*d6;
    if(vb<=0 && d2>=0 && d6<=0) return a+ac*(d2/(d2-d6));
    const float va=d3*d6-d5*d4;
    if(va<=0 && d4-d3>=0 && d5-d6>=0)
        return b+(c-b)*((d4-d3)/((d4-d3)+(d5-d6)));
    return a+ab*(vb/(va+vb+vc))+ac*(vc/(va+vb+vc));
}

// Closest-pair difference: first segment minus second segment.
template<class V> V segmentDelta(const V& p, const V& q, const V& a, const V& b) {
    const V u=q-p,v=b-a,r=p-a;
    const float uu=scalarProduct(u,u),vv=scalarProduct(v,v),uv=scalarProduct(u,v),ur=scalarProduct(u,r),vr=scalarProduct(v,r);
    float s=0,t=0;
    if(uu<=1e-12f) return p-segmentPoint(p,a,b);
    if(vv<=1e-12f) return segmentPoint(a,p,q)-a;
    const float denom=uu*vv-uv*uv;
    if(denom>1e-12f) s=std::clamp((uv*vr-vv*ur)/denom,0.0f,1.0f);
    t=(uv*s+vr)/vv;
    if(t<0) { t=0; s=std::clamp(-ur/uu,0.0f,1.0f); }
    else if(t>1) { t=1; s=std::clamp((uv-ur)/uu,0.0f,1.0f); }
    return p+u*s-(a+v*t);
}
template<class V> V triangleDelta(const V& p, const V& q,
                                  const V& a, const V& b, const V& c) {
    // An axis piercing the face has zero distance even if both endpoints
    // and all triangle edges are far from the intersection.
    const V direction=q-p, ab=b-a, ac=c-a;
    const V h=vectorProduct(direction,ac);
    const float det=scalarProduct(ab,h);
    if(std::abs(det)>1e-12f) {
        const V s=p-a, k=vectorProduct(s,ab);
        const float u=scalarProduct(s,h)/det,v=scalarProduct(direction,k)/det,t=scalarProduct(ac,k)/det;
        if(u>=0 && v>=0 && u+v<=1 && t>=0 && t<=1) return V(0,0,0);
    }
    V best=p-trianglePoint(p,a,b,c);
    for(const V d : {q-trianglePoint(q,a,b,c),segmentDelta(p,q,a,b),
                     segmentDelta(p,q,b,c),segmentDelta(p,q,c,a)})
        if(scalarProduct(d,d)<scalarProduct(best,best)) best=d;
    return best;
}

// Sweep a capsule axis by movement. Return a safe fraction in [0,1].
// Conservative advancement uses a separating plane: it cannot step over a
// thin triangle, unlike testing only the destination. Winding is irrelevant.
template<class V> float safeFraction(const V& bottom, const V& top, const V& movement,
                                    float radius, const V& a, const V& b, const V& c) {
    if(scalarProduct(movement,movement)<1e-12f) return 1.0f;
    const float skin=std::max(1e-5f,radius*0.001f);
    float t=0;
    for(int iteration=0;iteration<32;++iteration) {
        const V d=triangleDelta(bottom+movement*t,top+movement*t,a,b,c);
        const float distance=std::sqrt(scalarProduct(d,d));
        const float approach=-scalarProduct(d,movement);
        // Permit tangential movement and escape from an existing overlap.
        if(distance>1e-8f && approach<=0) return 1.0f;
        if(distance<=radius+skin) return t;
        const float closingSpeed=approach/distance;
        if(closingSpeed<=1e-8f) return 1.0f;
        const float advance=(distance-radius-skin)/closingSpeed;
        if(t+advance>=1.0f) return 1.0f;
        if(advance<=1e-7f) return t;
        t+=advance;
    }
    // Numerical convergence must never turn a possible contact into a miss.
    return t;
}
} // namespace wowee::rendering::capsule_sweep
