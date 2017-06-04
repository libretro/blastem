#version 110

attribute vec2 pos;
varying vec2 texcoord;
varying vec2 screencoord;
uniform float width, height;

void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	texcoord = sign(pos) * vec2(width/1024.0, height/-1024.0) + vec2(width/1024.0, height/1024.0);
	screencoord = sign(pos);
}