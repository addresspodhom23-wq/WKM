#version 450

layout(set = 0, binding = 0) uniform PerFrame {
    mat4 view;
    mat4 projection;
    mat4 lightSpaceMatrix;
    vec4 lightDir;
    vec4 lightColor;
    vec4 ambientColor;
    vec4 viewPos;
    vec4 fogColor;
    vec4 fogParams;
    vec4 shadowParams;
};

layout(location = 0) in vec3 aPos;
layout(location = 1) in vec4 aColor;
layout(location = 2) in float aSize;
layout(location = 3) in float aTile;
layout(location = 4) in float aSpin;
layout(location = 5) in vec3 aVelocity;
layout(location = 6) in float aTailSeconds;
layout(location = 7) in float aTailMode;

layout(location = 0) out vec4 vColor;
layout(location = 1) out float vTile;
layout(location = 2) out float vFogVisibility;
layout(location = 3) out vec2 vSpriteUV;

void main() {
    // One particle record is one INSTANCE. Four generated vertices form the
    // camera-facing quad; no largePoints feature or device point-size limit is
    // involved. aSize is the authored world-space HALF extent.
    const vec2 corners[4] = vec2[4](
        vec2(-1.0, -1.0),
        vec2( 1.0, -1.0),
        vec2(-1.0,  1.0),
        vec2( 1.0,  1.0)
    );
    vec2 corner = corners[gl_VertexIndex & 3];
    vec2 uvCorner = corner;

    vec4 centerView = view * vec4(aPos, 1.0);
    vec4 vertexView;

    if (aTailMode > 0.5) {
        vec3 tailView = mat3(view) * (-aVelocity * max(aTailSeconds, 0.0));
        float projectedLen2 = dot(tailView.xy, tailView.xy);
        if (projectedLen2 < 7.7e-4) {
            vertexView = centerView + vec4(corner * aSize, 0.0, 0.0);
        } else {
            vec3 axisUp = tailView * 0.5;
            vec2 perp = vec2(-tailView.y, tailView.x) *
                        (aSize * inversesqrt(projectedLen2));
            vec3 axisRight = vec3(perp, 0.0);
            vec3 tailCenter = centerView.xyz + axisUp;
            vertexView = vec4(
                tailCenter + axisRight * corner.x + axisUp * corner.y,
                centerView.w);
        }
    } else {
        float cs = cos(aSpin);
        float sn = sin(aSpin);
        corner = vec2(cs * corner.x - sn * corner.y,
                      sn * corner.x + cs * corner.y);
        vertexView = centerView + vec4(corner * aSize, 0.0, 0.0);
    }
    gl_Position = projection * vertexView;

    vColor = aColor;
    vTile = aTile;
    vSpriteUV = uvCorner * 0.5 + 0.5;

    float worldDist = length(viewPos.xyz - aPos);
    float fogRange = max(fogParams.y - fogParams.x, 0.001);
    vFogVisibility = clamp((fogParams.y - worldDist) / fogRange, 0.0, 1.0);
}
