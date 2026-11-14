// The flixel default fragment: texture times the colour transform, nothing else.
#pragma header

void main(void) {
	gl_FragColor = flixel_texture2D(bitmap, openfl_TextureCoordv);
}
