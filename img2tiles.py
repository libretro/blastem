#!/usr/bin/env python
from PIL import Image

def gchannel(Val):
	return (Val >> 4) & 0xE

threshold = 127

def get_rgba(im, pixels=None, idx=None, color=None):
	A = 255
	if color is None:
		color = pixels[idx]
	if type(color) == int:
		R, G, B = im.getpalette()[color*3:color*3+3]
	else:
		if len(color) == 4:
			R, G, B, A = color
		elif len(color) == 3:
			R, G, B = color
		else:
			L, A = color
			R = G = B = L
	return (R, G, B, A)

def get_gcolor(im, threshold, pixels=None, idx=None, color=None):
	R,G,B,A = get_rgba(im, pixels, idx, color)
	if A > threshold:
		return (gchannel(R), gchannel(G), gchannel(B))
	return 0

def get_color_info(im, pixels, rng, threshold, exclude={}):
	gencolors = {}
	A = 255
	pal = None
	transparent = False
	for idx in rng:
		gcolor = get_gcolor(im, threshold, pixels, idx)
		if type(gcolor) == tuple:
			if not gcolor in exclude:
				if gcolor in gencolors:
					gencolors[gcolor] += 1
				else:
					gencolors[gcolor] = 1
		else:
			transparent = True
	glist = [(gencolors[color], color) for color in gencolors]
	glist.sort()
	glist.reverse()
	return glist

def get_color_info_both(im, pixels, rng, threshold, exclude={}):
	gencolors = {}
	A = 255
	pal = None
	transparent = False
	for idx in rng:
		gcolor = get_gcolor(im, threshold, pixels, idx)
		if type(gcolor) == tuple:
			if not gcolor in exclude:
				if not gcolor in gencolors:
					_,best = best_match(gcolor, (exclude,))
					gencolors[gcolor] = (color_dist(gcolor, best), 1)
				else:
					(dist, count) = gencolors[gcolor]
					gencolors[gcolor] = (dist, count+1)
		else:
			transparent = True
	glist = [(gencolors[color][0] * gencolors[color][1], color) for color in gencolors]
	glist.sort()
	glist.reverse()
	
	return glist

def make_palette(im, trans_thresh, max_global, max_line):
	pixels = im.getdata()
	(width, height) = im.size
	colors = get_color_info(im, pixels, xrange(0, height * width), trans_thresh)
	print len(colors), 'distinct 9-bit colors in image'
	glob_pal = {}
	print 'Static Palette:'
	while len(glob_pal) < max_global and len(colors):
		idx = len(glob_pal)
		(count, color) = colors[0]
		print str(idx) + ':', color
		glob_pal[color] = idx
		colors = get_color_info_both(im, pixels, xrange(0, height * width), trans_thresh, glob_pal)
	line_pals = []
	if max_global < len(colors):
		for line in xrange(0, height):
			linestart = line * width
			if len(glob_pal):
				linecolors = get_color_info_both(im, pixels, xrange(linestart, linestart+width), trans_thresh, glob_pal)
			else:
				linecolors = get_color_info(im, pixels, xrange(linestart, linestart+width), trans_thresh)
			line_pal = {}
			while len(line_pal) < max_line and len(linecolors):
				(score, color) = linecolors[0]
				line_pal[color] = len(line_pal) + max_global
				if len(line_pal) < max_line:
					combo = dict(glob_pal)
					for color in line_pal:
						combo[color] = line_pal[color]
					linecolors = get_color_info_both(im, pixels, xrange(linestart, linestart+width), trans_thresh, combo)
			#for idx in xrange(0, min(max_line, len(linecolors))):
			#	(count, color) = linecolors[idx]
			#	line_pal[color] = idx + max_global
			line_pals.append(line_pal)
	return (glob_pal, line_pals, max_global, max_line)

def color_dist(a, b):
	(ra, ga, ba) = a
	(rb, gb, bb) = b
	return (ra-rb)**2 + (ga-gb)**2 + (ba-bb)**2

def best_match(gpixel, pals):
	bestdist = color_dist((0,0,0), (15, 15, 15))
	bestpalidx = 0
	bestcolor = (0,0,0)
	for i in xrange(0, len(pals)):
		pal = pals[i]
		for cur in pal:
			curdist = color_dist(gpixel, cur)
			if curdist < bestdist:
				bestdist = curdist
				bestpalidx = pal[cur]
				bestcolor = cur
	return (bestpalidx, bestcolor)

def trans_image(im, trans_thresh, pal, chunky):
	(global_pal, line_pals, _, _) = pal
	pixels = im.getdata()
	(width, height) = im.size
	gpixels = []
	A = 255
	pal = None
	x = 0
	y = 0
	numchannels = len(im.getbands())
	offset = 1 if numchannels == 4 or numchannels == 2 else 0
	for pixel in pixels:
		if x == width:
			x = 0
			y += 1
			if width % 8 and not chunky:
				for i in xrange(0, 8-(width%8)):
					gpixels.append(0)
		gpixel = get_gcolor(im, trans_thresh, color=pixel)
		if type(gpixel) == tuple:
			if gpixel in global_pal:
				val = global_pal[gpixel]
			elif gpixel in line_pals[y]:
				val = line_pals[y][gpixel]
			else:
				bestpal,color = best_match(gpixel, (global_pal, line_pals[y]))
				val = bestpal
			gpixels.append(offset+val)
		else:
			gpixels.append(gpixel)
		x += 1
	if width % 8 and not chunky:
		for i in xrange(0, 8-(width%8)):
			gpixels.append(0)
		width += 8-(width%8)
	if height % 8 and not chunky:
		for y in xrange(0, 8-(height%8)):
			for x in xrange(0, width):
				gpixels.append(0)
		height += 8-(height%8)

	return (width, height, gpixels)

def appendword(b, word):
	b.append(word >> 8)
	b.append(word & 0xff)

def to_tiles(palpix, raw=False, chunky=False, tile_height=8, sprite_order = False):
	(width, height, pixels) = palpix
	b = bytearray()
	if chunky:
		if not raw:
			appendword(b, width)
			appendword(b, height)
		for pixel in pixels:
			b.append(pixel)
	else:
		cwidth = width/8
		cheight = height/tile_height
		words = len(pixels)/4
		if not raw:
			appendword(b, words)
			appendword(b, cwidth)
			appendword(b, cheight)

		if sprite_order:
			for cx in xrange(0, cwidth):
				xstart = cx * 8
				for cy in xrange(0, cheight):
					startoff = cy*tile_height*width + xstart
					for row in xrange(0, tile_height):
						rowoff = startoff + row*width
						for bytecol in xrange(0, 4):
							boff = bytecol * 2 + rowoff
							#print 'boff:', boff, 'len(pixels)', len(pixels), 'cx', cx, 'cy', cy, 'cwidth', cwidth, 'cheight', cheight
							#print 'pixels[boff]:', pixels[boff]
							b.append(pixels[boff] << 4 | pixels[boff+1])
		else:
			for cy in xrange(0, cheight):
				ystart = cy*tile_height*width
				for cx in xrange(0, cwidth):
					startoff = (cx*8) + ystart
					for row in xrange(0, tile_height):
						rowoff = startoff + row*width
						for bytecol in xrange(0, 4):
							boff = bytecol * 2 + rowoff
							#print 'boff:', boff, 'len(pixels)', len(pixels), 'cx', cx, 'cy', cy, 'cwidth', cwidth, 'cheight', cheight
							#print 'pixels[boff]:', pixels[boff]
							b.append(pixels[boff] << 4 | pixels[boff+1])
	return b

def add_pal_entries(tiles, pal):
	(global_pal, line_pals, max_global, max_line) = pal
	tiles.append(max_global)
	tiles.append(max_line)
	pal_list = [(0, 0, 0)] * max_global
	for entry in global_pal:
		pal_list[global_pal[entry]] = entry
	for entry in pal_list:
		(R, G, B) = entry
		tiles.append(B)
		tiles.append(G << 4 | R)
	for line in line_pals:
		pal_list = [(0, 0, 0)] * max_line
		for entry in line:
			pal_list[line[entry]-max_global] = entry
		for entry in pal_list:
			(R, G, B) = entry
			tiles.append(B)
			tiles.append(G << 4 | R)



def main(argv):

	posargs = []
	omit_pal = raw = chunky = False
	expect_option = None
	options = {}
	tile_height = 8
	sprite_order = False
	for i in xrange(1, len(argv)):
		if argv[i].startswith('-'):
			if argv[i] == '-r':
				raw = True
			elif argv[i] == '-p':
				omit_pal = True
			elif argv[i] == '-o':
				sprite_order = True
			elif argv[i] == '-c':
				chunky = True
			elif argv[i] == '-i':
				tile_height = 16
			elif argv[i] == '-s' or argv[i] == '--spec':
				expect_option = 'specfile'
			else:
				print 'Unrecognized switch', argv[i]
				return
		elif not expect_option is None:
			options[expect_option] = argv[i]
			expect_option = None
		else:
			posargs.append(argv[i])
	if len(posargs) < 2 and not ('specfile' in options and len(posargs) >= 1):
		print "Usage: img2tiles.py [OPTIONS] infile outfile [STATIC_COLORS [DYNAMIC_COLORS]]"
		return
	if 'specfile' in options:
		props = open(options['specfile']).read().strip().split(',')
		fname,static_colors,dynamic_colors = props[0:3]
		for prop in props[3:]:
			if prop == 'chunky':
				chunky = True
			elif prop == 'raw':
				raw = True
			elif prop == 'nopal':
				omit_pal = True
			elif prop == 'interlace':
				tile_height = 16
			elif prop == 'sprite':
				sprite_order = True
		static_colors = int(static_colors)
		dynamic_colors = int(dynamic_colors)
		outfile = posargs[0]
	else:
		fname = posargs[0]
		outfile = posargs[1]
		static_colors = 8
		dynamic_colors = 8
		if len(posargs) > 2:
			static_colors = int(posargs[2])
			dynamic_colors = 16-static_colors
		if len(posargs) > 3:
			dynamic_colors = int(posargs[3])
	if dynamic_colors + static_colors > 16:
		print "No more than 16 combined dynamic and static colors are allowed"
		return
	im = Image.open(fname)
	pal = make_palette(im, threshold, static_colors, dynamic_colors)
	palpix = trans_image(im, threshold, pal, chunky)
	tiles = to_tiles(palpix, raw, chunky, tile_height, sprite_order)
	if not omit_pal:
		bits = add_pal_entries(tiles, pal)
	out = open(outfile, 'wb')
	out.write(tiles)

if __name__ == '__main__':
	import sys
	main(sys.argv)
