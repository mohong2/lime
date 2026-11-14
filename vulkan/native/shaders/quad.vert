#version 310 es
// lime-vulkan: 2D batch vertex shader.
//
// The input is pixel space (origin top left, y down); a push constant converts
// the screen size to NDC:
//   uProjection = (2/w, -2/h, -1, 1)
layout(location = 0) in vec2 aPosition;
layout(location = 1) in vec2 aTexCoord;
layout(location = 2) in vec4 aColor;
// Additive colour offset, already in -1..1 (SNORM bytes).
layout(location = 3) in vec4 aColorOffset;

layout(location = 0) out vec2 vTexCoord;
layout(location = 1) out vec4 vColor;
layout(location = 2) out vec4 vColorOffset;

layout(push_constant) uniform PushConstants {
	vec4 uProjection;
} pc;

void main() {

	gl_Position = vec4(aPosition * pc.uProjection.xy + pc.uProjection.zw, 0.0, 1.0);
	vTexCoord = aTexCoord;
	vColor = aColor;
	vColorOffset = aColorOffset;

}
