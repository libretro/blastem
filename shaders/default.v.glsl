
attribute vec2 pos;
varying mediump vec2 texcoord;
uniform mediump float width, height;

void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	texcoord = sign(pos) * vec2(width / 1024.0, height / -1024.0) + vec2(width / 1024.0, height / 1024.0);
}
