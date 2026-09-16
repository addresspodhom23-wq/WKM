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

    vec4 centerView = view * vec4(aPos, 1.0);
    vec4 vertexView = centerView + vec4(corner * aSize, 0.0, 0.0);
    gl_Position = projection * vertexView;

    vColor = aColor;
    vTile = aTile;
    vSpriteUV = corner * 0.5 + 0.5;

    float worldDist = length(viewPos.xyz - aPos);
    float fogRange = max(fogParams.y - fogParams.x, 0.001);
    vFogVisibility = clamp((fogParams.y - worldDist) / fogRange, 0.0, 1.0);
}
