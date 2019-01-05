#!/usr/bin/env python

from zipfile import ZipFile
from sys import exit, argv

def detect_rise(last, sample, bit):
	mask = 1 << bit
	return (not last & mask) and (sample & mask)

def detect_fall(last, sample, bit):
	mask = 1 << bit
	return (last & mask) and (not sample & mask)

def detect_high(sample, bit):
	mask = 1 << bit
	return sample & mask

def detect_low(sample, bit):
	mask = 1 << bit
	return not sample & mask
	
def get_value(sample, bits):
	value = 0
	for i in xrange(0, len(bits)):
		bit = bits[i]
		value |= (sample >> bit & 1) << i
	return value
	
def swizzle_mode4(row, col):
	return (col & 1) | (row << 1) | (col << 8 & 0xFE00)

def analyze_delays(chanmap, datafile):
	if 'M68K_CLK' in chanmap:
		m68k_clk = chanmap['M68K CLK']
	elif 'CLK' in chanmap:
		m68k_clk = chanmap['CLK']
	m_as = chanmap['!AS']
	ram_oe = chanmap['RAM !LOE/!RFSH']
	ram_ce = chanmap['RAM !CE']
	last = False
	prev = False
	prevRefresh = False
	clks = 0
	as_start = 0
	for line in datafile.readlines():
		line = line.strip()
		if line and not line.startswith(';'):
			sample,_,num = line.partition('@')
			sample = int(sample, 16)
			if not (last is False):
				if detect_rise(last, sample, m68k_clk):
					clks = clks + 1
				if detect_rise(last, sample, m_as):
					as_clks  = clks - as_start
					if as_clks > 2:
						if not (prev is False):
							print '!AS held for', as_clks, 'cycles starting (delay of ' + str(as_clks - 2) + ') at', as_start, 'and ending at', clks, 'delta since last delay:', as_start - prev
						else:
							print '!AS held for', as_clks, 'cycles starting (delay of ' + str(as_clks - 2) + ') at', as_start, 'and ending at', clks
						prev = as_start
				elif detect_fall(last, sample, m_as):
					as_start = clks
				if detect_fall(last, sample, ram_oe) and detect_high( sample, ram_ce):
					if prevRefresh is False:
						print 'RAM refresh at ', clks
					else:
						print 'RAM refresh at', clks, 'delta since last:', clks-prevRefresh
					prevRefresh = clks
			last = sample
			
def analyze_refresh(chanmap, datafile):
	if 'M68K_CLK' in chanmap:
		m68k_clk = chanmap['M68K CLK']
	elif 'CLK' in chanmap:
		m68k_clk = chanmap['CLK']
	ram_oe = chanmap['RAM !LOE/!RFSH']
	ram_ce = chanmap['RAM !CE']
	clks = 0
	last = False
	prevRefresh = False
	for line in datafile.readlines():
		line = line.strip()
		if line and not line.startswith(';'):
			sample,_,num = line.partition('@')
			sample = int(sample, 16)
			if not (last is False):
				if detect_rise(last, sample, m68k_clk):
					clks = clks + 1
				if detect_fall(last, sample, ram_oe) and detect_high( sample, ram_ce):
					if prevRefresh is False:
						print 'RAM refresh at ', clks
					else:
						print 'RAM refresh at', clks, 'delta since last:', clks-prevRefresh
					prevRefresh = clks
			last = sample
			
			
table_start = 0x3800
table_end = table_start + 0x600
sat_start = 0x3E00 #0x3F00 
sat_xname = sat_start + 0x80
sat_end = sat_start + 0x100


def analyze_vram(chanmap, datafile):
	address_bits = [chanmap['AD{0}'.format(i)] for i in xrange(0, 8)]
	ras = chanmap['!RAS']
	cas = chanmap['!CAS']
	hsync = chanmap['!HSYNC']
	state = 'begin'
	last = False
	for line in datafile.readlines():
		line = line.strip()
		if line and not line.startswith(';'):
			sample,_,num = line.partition('@')
			sample = int(sample, 16)
			if not (last is False):
				if detect_fall(last, sample, hsync):
					print 'HSYNC low @ {0}'.format(num)
				elif detect_rise(last, sample, hsync):
					print 'HSYNC high @ {0}'.format(num)
				if state == 'begin':
					if detect_fall(last, sample, ras):
						state = 'ras'
						row = get_value(sample, address_bits)
					elif detect_fall(last, sample, cas):
						state = 'cas'
				elif state == 'ras':
					if detect_fall(last, sample, cas):
						col = get_value(sample, address_bits)
						address = swizzle_mode4(row, col)
						
						if address < table_end and address >= table_start:
							offset = (address - table_start)/2
							desc = 'Map Row {0} Col {1}'.format(offset / 32, offset & 31)
						elif address >= sat_start and address < sat_xname:
							offset = address - sat_start
							desc = 'Sprite {0} Y Read'.format(offset)
						elif address >= sat_xname and address < sat_end:
							offset = address - sat_xname
							desc = 'Sprite {0} X/Name Read'.format(offset / 2)
						else:
							desc = 'Tile {0} Row {1}'.format(address / 32, ((address / 4) & 7) + (0.5 if address & 2 else 0))
						print '{0:02X}:{1:02X} - {2:04X} @ {3} - {4}'.format(row, col, address, num, desc)
						state = 'begin'
				elif state == 'cas':
					if detect_fall(last, sample, ras):
						print 'refresh @ {0}'.format(num)
						state = 'begin'
			last = sample
			
def analyze_z80_mreq(chanmap, datafile):
	m1 = chanmap['!M1']
	mreq = chanmap['!MREQ']
	addressMask = 0x3FF
	last = None
	lastWasM1 = False
	for line in datafile.readlines():
		line = line.strip()
		if line and not line.startswith(';'):
			sample,_,num = line.partition('@')
			sample = int(sample, 16)
			if not (last is None):
				if detect_rise(last, sample, mreq):
					address = last & addressMask
					if detect_low(last, m1):
						print 'M1 read {0:02X} @ {1}'.format(address, num)
						lastWasM1 = True
					elif lastWasM1:
						print 'Refresh {0:02X} @ {1}'.format(address, num)
						lastWasM1 = False
					else:
						print 'Access {0:02X} @ {1}'.format(address, num)
			last = sample

def main(args):
	if len(args) < 2:
		print 'Usage: analyze_olp.py filename'
		exit(1)
	olpfile = ZipFile(args[1], "r")
	channelfile = olpfile.open('channel.labels')
	channels = [line.strip() for line in channelfile.readlines()]
	channelfile.close()
	print channels
	chanmap = {}
	for i in xrange(0, len(channels)):
		chanmap[channels[i]] = i
	datafile = olpfile.open('data.ols')
	#analyze_delays(chanmap, datafile)
	#analyze_vram(chanmap, datafile)
	#analyze_refresh(chanmap, datafile)
	analyze_z80_mreq(chanmap, datafile)
	datafile.close()
	

if __name__ == '__main__':
	main(argv)
