#version 450

// M2 ribbon emitter fragment shader.
// Samples the ribbon texture, multiplied by vertex color and alpha.
// Uses additive blending (pipeline-level) for magic/spell trails.

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

layout(location = 0) in vec3 vColor;
layout(location = 1) in float vAlpha;
layout(location = 2) in vec2 vUV;
layout(location = 3) in float vFogFactor;
layout(location = 4) flat in int vFogPolicy;

layout(location = 0) out vec4 outColor;

void main() {
    vec4 tex = texture(uTexture, vUV);
    // For additive ribbons alpha comes from texture luminance; multiply by vertex alpha.
    float a = tex.a * vAlpha;
    if (a < 0.01) discard;
    vec3 rgb = tex.rgb * vColor;
    // Classic ribbon fog follows the material: Unfogged is untouched,
    // ordinary alpha approaches scene fog colour, Add/AddAlpha approach black.
    if (vFogPolicy != 0) {
        vec3 target = vFogPolicy == 2 ? vec3(0.0) : fogColor.rgb;
        rgb = mix(target, rgb, vFogFactor);
    }
    outColor = vec4(rgb, a);
}
