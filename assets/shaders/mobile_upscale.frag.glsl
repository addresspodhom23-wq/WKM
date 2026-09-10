#version 450

// Mobile fast upscale.  The scene sampler is linear, so one filtered lookup
// performs bilinear reconstruction in hardware.  It deliberately shares the
// FSR descriptor and push-constant layout, allowing the renderer to switch the
// fragment module without another pipeline or descriptor implementation.

layout(set = 0, binding = 0) uniform sampler2D uInput;

layout(push_constant) uniform FSRConstants {
    vec4 con0;
    vec4 con1;
    vec4 con2;
    vec4 con3;
} fsr;

layout(location = 0) in vec2 TexCoord;
layout(location = 0) out vec4 outColor;

void main() {
    outColor = vec4(texture(uInput, TexCoord).rgb, 1.0);
}
