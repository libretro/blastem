
uniform sampler2D textures[2];

varying mediump vec2 texcoord;

void main()
{
	mediump float x0 = (floor(texcoord.x * 512.0 - 0.25) + 0.5)/512.0;
	mediump float x1 = (floor(texcoord.x * 512.0 + 0.25) + 0.5)/512.0;
	mediump float y0 = (floor(texcoord.y * 512.0 + 0.25) + 0.5)/512.0;
	mediump float y1 = (floor(texcoord.y * 512.0 - 0.25) + 0.5)/512.0;
	
	
	mediump vec2 modifiedCoord0 = vec2(texcoord.x, (floor(texcoord.y * 512.0 + 0.25) + 0.5)/512.0);
	mediump vec2 modifiedCoord1 = vec2(texcoord.x, (floor(texcoord.y * 512.0 - 0.25) + 0.5)/512.0);
	mediump float ymix = (sin(texcoord.y * 1024.0 * 3.14159265359) + 1.0) * 0.5;
	mediump float xmix = (sin(texcoord.x * 1024.0 * 3.14159265359) + 1.0) * 0.5;
	gl_FragColor = mix(
		mix(
			texture2D(textures[1], vec2(x0, y1)),
			texture2D(textures[0], vec2(x0, y0)),
			ymix
		),
		mix(
			texture2D(textures[1], vec2(x1, y1)),
			texture2D(textures[0], vec2(x1, y0)),
			ymix
		),
		xmix
	);
}
