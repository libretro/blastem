
uniform sampler2D textures[2];

varying mediump vec2 texcoord;

void main()
{
	mediump vec2 modifiedCoord0 = vec2(texcoord.x, (floor(texcoord.y * 512.0 + 0.25) + 0.5)/512.0);
	mediump vec2 modifiedCoord1 = vec2(texcoord.x, (floor(texcoord.y * 512.0 - 0.25) + 0.5)/512.0);
	gl_FragColor = mix(
		texture2D(textures[1], modifiedCoord1),
		texture2D(textures[0], modifiedCoord0),
		(sin(texcoord.y * 1024.0 * 3.14159265359) + 1.0) * 0.5
	);
}
