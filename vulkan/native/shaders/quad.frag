#version 310 es
// lime-vulkan: 2D batch fragment shader.
//
// The output is **premultiplied alpha**: rgb is the texture times the vertex
// colour, and the whole result is then multiplied by the vertex alpha.
//
// Why premultiplied: the textures are premultiplied (lime bitmaps are) and the
// vertex alpha takes part in the blend. Skipping it looks identical for NORMAL
// with SRC_ALPHA / ONE_MINUS_SRC_ALPHA, but is fatal for SCREEN
// (ONE / ONE_MINUS_SRC_COLOR), which ignores alpha entirely: a glow texture
// whose premultiplied rgb is near zero turns into a bright blob once the
// premultiplication is undone (measured: a white disc appeared behind the logo
// of the boot splash).
precision mediump float;

layout(location = 0) in vec2 vTexCoord;
layout(location = 1) in vec4 vColor;
layout(location = 2) in vec4 vColorOffset;

layout(location = 0) out vec4 oColor;

layout(set = 0, binding = 0) uniform sampler2D uTexture;

void main() {

	vec4 texel = texture (uTexture, vTexCoord);

	// Same order as OpenFL's color transform: multiply, then add, then clamp.
	// vColorOffset is zero for everything that has no colorTransform.
	//
	// The offset is scaled by the final alpha because the texture is premultiplied
	// and the offset is not: adding it unscaled leaves a fully transparent texel
	// with the offset colour and a zero alpha, and the premultiplied blend adds
	// that rgb anyway. That is how a colour transform turns a whole sprite into a
	// solid block (WBG.lua: characters tinted to white on a white background).
	vec4 result;

	result.a = clamp (texel.a * vColor.a + vColorOffset.a, 0.0, 1.0);
	result.rgb = clamp (texel.rgb * vColor.rgb + vColorOffset.rgb * result.a, 0.0, 1.0) * vColor.a;

	oColor = result;

}
