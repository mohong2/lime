// The FlxGraphicsShader vertex shader, written exactly as the runtime receives it.
// OpenFL expands #pragma header / #pragma body while Haxe compiles; the pragmas
// are kept here so the translator's expansion path is covered as well.
#pragma header

attribute float alpha;
attribute vec4 colorMultiplier;
attribute vec4 colorOffset;
uniform bool hasColorTransform;

void main(void) {
	#pragma body

	openfl_Alphav = openfl_Alpha * alpha;

	if (hasColorTransform) {
		openfl_ColorOffsetv = colorOffset / 255.0;
		openfl_ColorMultiplierv = colorMultiplier;
	}
}
