
attribute vec2 pos;
varying mediump vec2 texcoord;
uniform mediump float width, height;
uniform mediump vec2 texsize;

void main()
{
	gl_Position = vec4(pos, 0.0, 1.0);
	texcoord = sign(pos) * vec2(0.5 * width / texsize.x, -0.5 * height / texsize.y) + vec2(0.5 * width / texsize.x, 0.5 * height / texsize.y);
}
