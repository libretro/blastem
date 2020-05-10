/* Subtle CRT shader usable in fullscreen - Anaël Seghezzi [anael(at)maratis3d.com]
   This shader is free software distributed under the terms of the GNU General Public
   License version 3 or higher. This gives you the right to redistribute and/or
   modify the program as long as you follow the terms of the license. See the file
   COPYING for full license details.
*/

#define M_PI 3.14159265358979323846

uniform sampler2D textures[2];
uniform mediump float width, height;
uniform mediump vec2 texsize;
varying mediump vec2 texcoord;
varying mediump vec2 screencoord;


mediump float nrand(vec2 n) {
	return fract(sin(dot(n.xy, vec2(12.9898, 78.233))) * 43758.5453);
}

mediump float scanline(vec2 texco)
{
	return (1.0 - abs(cos(texco.y * texsize.y * M_PI)));
}

mediump vec2 sharp_coord(mediump vec2 texco, mediump vec2 dim, mediump vec2 sharpness)
{
	mediump vec2 texcoif = texco * dim;
	mediump vec2 texcoi = floor(texcoif);
	mediump vec2 mu = (texcoif - 0.5) - texcoi;
	mediump vec2 mub = pow(abs(mu) * 2.0, sharpness) * sign(mu) * 0.5;
	return (texcoi + mub + 0.5) / dim;	
}

void main()
{
	mediump float v = 1.0 / texsize.y;
	mediump float yforce = 0.175;
	mediump float vign = length(screencoord);

	// monitor deformation
	mediump vec2 monitorcoord = (screencoord + screencoord * vign * 0.025);
	
	if (monitorcoord.x < -1.0 || monitorcoord.y < -1.0 || monitorcoord.x > 1.0 || monitorcoord.y > 1.0) {
		gl_FragColor = vec4(0.0);
		return;
	}

	mediump vec2 texco = monitorcoord * vec2(0.5*width/texsize.x, -0.5 * height/texsize.y) + vec2(0.5*width/texsize.x, 0.5*height/texsize.y);

	// mask
	mediump float maskx = 1.0 - pow(abs(monitorcoord.x), 200.0);
	mediump float masky = 1.0 - pow(abs(-monitorcoord.y), 200.0);
	mediump float mask = clamp(maskx * masky, 0.0, 1.0);

	// sharp texcoord
	mediump vec2 texco_sharp0 = sharp_coord(texco, texsize, vec2(4.0, 8.0));
	mediump vec2 texco_sharp1 = sharp_coord(texco - vec2(0.0, 0.5 / texsize.y), texsize, vec2(4.0, 8.0));

	mediump vec4 src0 = texture2D(textures[0], texco_sharp0);
	mediump vec4 src1 = texture2D(textures[1], texco_sharp1);

	// interlace mix
	mediump float interlace = cos((texco.y * 2.0 * texsize.y) * M_PI);
	mediump vec4 src_mix = mix(src0, src1, interlace * 0.5 + 0.5);

	// blur
	mediump vec4 src_blur = mix(texture2D(textures[0], texco), texture2D(textures[1], texco), 0.5);

#ifdef NO_SCANLINE

	gl_FragColor = (src_mix * 0.95 + (src_blur * (1.6 - vign * 0.4) * 0.1)) * mask;

#else
	// multisample scanline with grain
	// TODO: offset grain with time (needs a "frame" uniform)
	mediump float cosy;
	cosy  = scanline(texco + vec2(0.125, v * (nrand(texcoord + vec2(0.0, 512.0/texsize.y)) * 0.25) + 512.0*0.3333/texsize.y));
	cosy += scanline(texco + vec2(0.25, v * (nrand(texcoord + vec2(0.0, 1024.0/texsize.y)) * 0.25) + 512.0*0.25/texsize.y));
	cosy += scanline(texco + vec2(0.50, v * (nrand(texcoord + vec2(0.0, 1536.0/texsize.y)) * 0.25) + 512.0*0.6666/texsize.y));
	cosy += scanline(texco + vec2(0.75, v * (nrand(texcoord + vec2(0.0, 2048.0/texsize.y)) * 0.25) + 512.0*0.75/texsize.y));
	cosy *= 0.25;

	// final scanline + burn
	gl_FragColor = ((src_mix * ((1.0 - yforce) + cosy * yforce)) + (src_blur * (1.6 - vign * 0.4) * 0.1)) * mask;

#endif
}