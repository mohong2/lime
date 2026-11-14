// An fbm style shader: for loops, macros and array uniforms, the heavy case.
// The point is to prove that a complex shader translates and runs at frame rate,
// not to be asserted per pixel.
#pragma header

#define OCTAVES 5

uniform float iTime;
uniform vec2 iResolution;
uniform vec4 uArray[4];

float hash(vec2 p) {
	return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float noise(vec2 p) {
	vec2 i = floor(p);
	vec2 f = fract(p);
	f = f * f * (3.0 - 2.0 * f);

	float a = hash(i);
	float b = hash(i + vec2(1.0, 0.0));
	float c = hash(i + vec2(0.0, 1.0));
	float d = hash(i + vec2(1.0, 1.0));

	return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main(void) {
	vec2 uv = openfl_TextureCoordv * 4.0;
	vec4 source = texture2D(bitmap, openfl_TextureCoordv);

	float value = 0.0;
	float amplitude = 0.5;

	for (int i = 0; i < OCTAVES; ++i) {
		value += noise(uv + iTime * 0.15) * amplitude;
		uv *= 2.0;
		amplitude *= 0.5;
	}

	float glow = value * uArray[0].x + uArray[1].y + uArray[2].z + uArray[3].w;

	gl_FragColor = vec4(source.rgb * glow * openfl_Alphav, source.a * openfl_Alphav);
}
