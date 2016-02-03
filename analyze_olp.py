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
	analyze_delays(chanmap, datafile)
	datafile.close()
	#datafile = olpfile.open('data.ols')
	#analyze_refresh(chanmap, datafile)

if __name__ == '__main__':
	main(argv)
