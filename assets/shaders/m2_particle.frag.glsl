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

layout(set = 1, binding = 0) uniform sampler2D uTexture;

layout(push_constant) uniform Push {
    vec2 tileCount;
    int alphaKey;
    int vanillaRendering;
    int fogPolicy;
} push;

layout(location = 0) in vec4 vColor;
layout(location = 1) in float vTile;
layout(location = 2) in float vFogVisibility;
layout(location = 3) in vec2 vSpriteUV;

layout(location = 0) out vec4 outColor;

void main() {
    vec2 p = vSpriteUV;
    float tile = floor(vTile);
    float tx = mod(tile, push.tileCount.x);
    float ty = floor(tile / push.tileCount.x);
    vec2 uv = (vec2(tx, ty) + p) / push.tileCount;
    vec4 texColor = texture(uTexture, uv);

    if (push.alphaKey != 0) {
        float lum = dot(texColor.rgb, vec3(0.299, 0.587, 0.114));
        if (lum < 0.05) discard;
    }

    // Vanilla's particle is a square billboard. Its BLP alpha defines the
    // silhouette; a synthetic circular mask clips authored smoke/flame cells.
    float edge = push.vanillaRendering != 0
        ? 1.0
        : 1.0 - smoothstep(0.4, 0.5, length(p - 0.5));
    float alpha = texColor.a * vColor.a * edge;
    // Pipelines use straight-alpha blending (SRC_ALPHA for Blend/AddAlpha).
    // Premultiplying RGB here and then applying SRC_ALPHA again produces
    // alpha-squared particles: dim smoke, weak flames and undersaturated glows.
    vec3 rgb = texColor.rgb * vColor.rgb;

    if (push.vanillaRendering != 0) {
        // Classic particle fog changes RGB only. 0x08 disables it, ordinary
        // particles approach the scene fog colour, and Add/AddAlpha approach
        // black so their additive energy vanishes into the distance.
        if (push.fogPolicy != 0) {
            vec3 target = push.fogPolicy == 2 ? vec3(0.0) : fogColor.rgb;
            rgb = mix(target, rgb, vFogVisibility);
        }
    } else {
        // Keep the historical non-Vanilla particle behaviour.
        alpha *= vFogVisibility;
    }
    outColor = vec4(rgb, alpha);
}
