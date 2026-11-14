// RGBPalette: retints through three mask channels (red, green, blue).
#pragma header

uniform vec3 r;
uniform vec3 g;
uniform vec3 b;
uniform float mult;

void main(void) {
	vec4 tex = texture2D(bitmap, openfl_TextureCoordv);

	vec3 mask = step(vec3(0.5), tex.rgb);

	vec3 tint = r * mask.r * (1.0 - mask.g) * (1.0 - mask.b)
		+ g * (1.0 - mask.r) * mask.g * (1.0 - mask.b)
		+ b * (1.0 - mask.r) * (1.0 - mask.g) * mask.b;

	gl_FragColor = vec4(tint * mult * openfl_Alphav, tex.a * openfl_Alphav);
}
