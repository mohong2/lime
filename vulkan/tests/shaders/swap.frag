// ColorSwap: replaces pixels close to uColor1 with uColor2.
// Mirrors Psych's ColorSwapShader (which also shifts hue; only the part that can
// be asserted per pixel is kept here).
#pragma header

uniform vec3 uColor1;
uniform vec3 uColor2;
uniform float uThreshold;

void main(void) {
	vec4 color = texture2D(bitmap, openfl_TextureCoordv);

	if (distance(color.rgb, uColor1) < uThreshold) {
		color.rgb = uColor2;
	}

	gl_FragColor = vec4(color.rgb * openfl_Alphav, color.a * openfl_Alphav);
}
