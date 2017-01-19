#version 110

uniform sampler2D textures[2];

varying vec2 texcoord;

void main()
{
	gl_FragColor = mix(
		texture2D(textures[0], texcoord),
		texture2D(textures[1], vec2(texcoord.x, texcoord.y - 1.0/1024.0)),
		sin((texcoord.y * 1024.0 - 0.75) * 3.14159265359) / 2.0 + 0.5
	);
}
