// Vertex displacement for the WiggleEffect. The amount comes from a uniform, which makes it assertable per pixel.
#pragma header

uniform vec2 uOffset;
uniform float uAmplitude;
uniform float uFrequency;

void main(void) {
	#pragma body

	vec4 position = openfl_Position;

	position.x += uOffset.x;
	position.y += uOffset.y + sin(position.x * uFrequency) * uAmplitude;

	gl_Position = openfl_Matrix * position;
}
