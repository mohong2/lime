#version 310 es
// lime-vulkan: test pattern fragment shader.
// All colours come from the vertex buffer; the only thing added here is a slow
// breathing, so that 'the image is alive' is visible at a glance.
precision mediump float;

layout(location = 0) in vec3 vColor;
layout(location = 0) out vec4 oColor;

layout(push_constant) uniform PushConstants {
	float uTime;
	float uRotate;
} pc;

void main() {

	float pulse = 0.86 + 0.14 * sin(pc.uTime * 1.7);
	oColor = vec4(vColor * pulse, 1.0);

}
