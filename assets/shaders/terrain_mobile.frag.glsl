#version 450

// Mobile terrain baseline for tile-based GPUs. This diagnostic version samples
// only the base layer. The previous mobile pass proved that the desktop ALU
// work was expensive, but its remaining seven texture reads still cost roughly
// 50-63 ms on Adreno 618. If this one-read baseline removes that cost, the
// production path will pre-bake the four WoW layers into one mobile texture.

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
    vec4 playerPos;
    vec4 playerWake;
    vec4 localLightPosRadius[64];
    vec4 localLightColorIntensity[64];
    ivec4 localLightMeta;
};

layout(set = 1, binding = 0) uniform sampler2D uBaseTexture;
layout(set = 1, binding = 1) uniform sampler2D uLayer1Texture;
layout(set = 1, binding = 2) uniform sampler2D uLayer2Texture;
layout(set = 1, binding = 3) uniform sampler2D uLayer3Texture;
layout(set = 1, binding = 4) uniform sampler2D uLayer1Alpha;
layout(set = 1, binding = 5) uniform sampler2D uLayer2Alpha;
layout(set = 1, binding = 6) uniform sampler2D uLayer3Alpha;

layout(set = 1, binding = 7) uniform TerrainParams {
    int layerCount;
    int hasLayer1;
    int hasLayer2;
    int hasLayer3;
};

// Retain set 0's descriptor interface even though this shader deliberately
// never samples the map. That keeps the existing pipeline layout compatible.
layout(set = 0, binding = 1) uniform sampler2DShadow uShadowMap;

layout(location = 0) in vec3 FragPos;
layout(location = 1) in vec3 Normal;
layout(location = 2) in vec2 TexCoord;
layout(location = 3) in vec2 LayerUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 colour = texture(uBaseTexture, TexCoord);

    vec3 normal = normalize(Normal);
    vec3 toLight = normalize(-lightDir.xyz);
    float diffuseAmount = max(dot(normal, toLight), 0.0);
    vec3 lit = colour.rgb *
        (ambientColor.rgb + diffuseAmount * lightColor.rgb);

    float distanceToCamera = length(viewPos.xyz - FragPos);
    float fogRange = max(fogParams.y - fogParams.x, 0.001);
    float fogAmount = clamp((fogParams.y - distanceToCamera) / fogRange,
                            0.0, 1.0);
    outColor = vec4(mix(fogColor.rgb, lit, fogAmount), 1.0);
}
