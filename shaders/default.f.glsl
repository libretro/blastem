#version 110

uniform sampler2D textures[2];

varying vec2 texcoord;

void main()
{
	vec2 modifiedCoord = vec2(texcoord.x, (floor(texcoord.y * 512.0) + 0.5)/512.0);
	gl_FragColor = mix(
		texture2D(textures[1], modifiedCoord),
		texture2D(textures[0], modifiedCoord),
		(sin(texcoord.y * 1024.0 * 3.14159265359) + 1.0) * 0.5
	);
}
