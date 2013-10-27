#version 110

uniform sampler2D textures[2];

varying vec2 texcoord;

void main()
{
	gl_FragColor = texture2D(textures[0], texcoord);
}
