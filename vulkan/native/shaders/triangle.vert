#version 310 es
// lime-vulkan: test pattern vertex shader.
//
// Two push constants:
//   uTime   -- animation time in seconds
//   uRotate -- 1 rotates (triangle pattern), 0 does not (the full screen
//              gradient has to fill the window and must not spin)
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec3 aColor;

layout(location = 0) out vec3 vColor;

layout(push_constant) uniform PushConstants {
	float uTime;
	float uRotate;
} pc;

void main() {

	float angle = pc.uTime * pc.uRotate;
	float c = cos(angle);
	float s = sin(angle);
	mat2 rotation = mat2(c, s, -s, c);

	gl_Position = vec4(rotation * aPosition, 0.0, 1.0);
	vColor = aColor;

}
