#!/usr/bin/env python

#OLD
#0 - !SE
#1 - !CAS
#2 - A0
#3 - A1
#------
#4 - A2
#5 - A3
#6 - A7
#7 - EDCLK
#------
#8 - !HSYNC
#9 - A4
#A - A5
#B - A6
#------
#C - !RAS
#D - !WB/!WE
#E - !DT/!OE
#F - SC

#NEW
#0 - !IPL2
#1 - !CAS
#2 - A0
#3 - A1
#------
#4 - A2
#5 - A3
#6 - A7
#7 - !HSYNC
#------
#8 - !VSYNC
#9 - A4
#A - A5
#B - A6
#------
#C - !RAS
#D - !WB/!WE
#E - !DT/!OE
#F - SC


#VRAM swizzling
#A0 = V0
#A1 = V1
#A8 = V2
#A9 = V3
#A10 = V4
#A11 = V5
#A12 = V6
#A13 = V7
#A14 = V8
#A15 = V9
#--guesses follow--
#A2 = V10
#A3 = V11
#A4 = V12
#A5 = V13
#A6 = V14
#A7 = V15


def get_addr(sample):
	return ((sample >> 2) & 0xF) | ((sample >> 5) & 0x70) | ((sample << 1) & 0x80)

def swizzle_addr(addr):
	return (addr & 0x0003) | ((addr >> 6) & 0x03FC) | ((addr << 8) & 0xFC00)

def print_addr_op(addr, addr_format, mode, samplenum, triggerpos, rate):
	print '{0:{1}} ({2:{1}}) {3}@{4} ns'.format(swizzle_addr(addr), addr_format, addr, mode, (samplenum - triggerpos)*rate)

def detect_rise(last, sample, bit):
	mask = 1 << bit
	return (not last & mask) and (sample & mask)
	
def detect_fall(last, sample, bit):
	mask = 1 << bit
	return (last & mask) and (not sample & mask)

def detect_high(sample, bit):
	mask = 1 << bit
	return sample & mask


ipl2 = 0x0
cas = 0x1
ras = 0xC
vsync = 0x8
hsync = 0x7
wewb = 0xD
oedt = 0xE
sc = 0xF

last = False
state = 'begin'
triggerpos = 0
readcounter = 0
sillyread = 0
lastaddr = -1
edclk_ticks = 0
sc_ticks = 0
tick_start = False
#f = open('street_fighter_vram_100mhz_hsync_trig_2.ols')
#f = open('street_fighter_vram_50mhz_hsync_trig.ols')
from sys import argv,exit
if len(argv) < 2:
	print 'usage: analyze.py filename'
	exit(1)
if '-b' in argv:
	addr_format = '016b'
else:
	addr_format = '04X'
f = open(argv[1])
for line in f:
	if line.startswith(';TriggerPosition'):
		_,_,triggerpos = line.partition(':')
		triggerpos = int(triggerpos.strip())
	elif line.startswith(';Rate'):
		_,_,rate = line.partition(':')
		#convert to nanoseconds between samples
		rate = (1.0/float(rate.strip())) * 1000000000.0
	elif not line.startswith(';'):
		sample,_,samplenum = line.partition('@')
		samplenum = int(samplenum.strip())
		sample = int(sample, 16)
		if detect_rise(last, sample, sc):
			sc_ticks += 1
		if not (last is False):
			#detect falling edge of !HSYNC
			if detect_fall(last, sample, hsync):
				if readcounter:
					print readcounter, 'reads,', sillyread, 'redundant reads'
					readcounter = sillyread = 0
				if not tick_start is False:
					print 'SC:', sc_ticks, ' ticks, {0}MHz'.format(float(sc_ticks)/((rate * (samplenum-tick_start)) / 1000.0))
				tick_start = samplenum
				edclk_ticks = sc_ticks = 0
				print 'HSYNC Start @ {0} ns'.format((samplenum - triggerpos)*rate)
			#detect rising edge of !HSYNC
			elif detect_rise(last, sample, hsync):
				if not tick_start is False:
					float(edclk_ticks)/((rate * (samplenum-tick_start)) / 1000.0)
					print 'EDCLK:', edclk_ticks, ' ticks, {0}MHz'.format(float(edclk_ticks)/((rate * (samplenum-tick_start)) / 1000.0))
					print 'SC:', sc_ticks, ' ticks, {0}MHz'.format(float(sc_ticks)/((rate * (samplenum-tick_start)) / 1000.0))
				tick_start = samplenum
				edclk_ticks = sc_ticks = 0
				print 'HSYNC End @ {0} ns'.format((samplenum - triggerpos)*rate)
			if detect_fall(last, sample, vsync):
				print 'VSYNC Start @ {0} ns'.format((samplenum - triggerpos)*rate)
			elif detect_rise(last, sample, vsync):
				print 'VSYNC End @ {0} ns'.format((samplenum - triggerpos)*rate)
			if detect_fall(last, sample, ipl2):
				print 'IPL2 Low @ {0} ns'.format((samplenum - triggerpos)*rate)
			elif detect_rise(last, sample, ipl2):
				print 'IPL2 High @ {0} ns'.format((samplenum - triggerpos)*rate)
			if state == 'begin':
				#detect falling edge of !RAS
				if detect_fall(last, sample, ras):
					state = 'ras'
					row = get_addr(sample)
					mode = 'ram' if detect_high(sample, oedt) else 'read transfer'
				elif detect_fall(last, sample, cas) and detect_high(sample, oedt):
					state = 'cas'
			elif state == 'ras':
				if detect_fall(last, sample, cas):
					state = 'begin'
					col = get_addr(sample)
					addr = (row << 8) | col
					if mode == 'ram':
						state = 'ras_cas'
					else:
						print_addr_op(addr, addr_format, mode, samplenum, triggerpos, rate)
					lastaddr = addr
					#print '{0:04X} {1} - {2:02X}:{3:02X} - {0:016b}'.format(addr, mode, row, col)
			elif state == 'cas':
				if detect_fall(last, sample, ras):
					state = 'begin'
					print 'refresh@{0} ns'.format((samplenum - triggerpos)*rate)
			elif state == 'ras_cas':
				if detect_fall(last, sample, oedt):
					readcounter += 1
					if addr == lastaddr:
						sillyread += 1
					print_addr_op(addr, addr_format, 'read', samplenum, triggerpos, rate)
					state = 'begin'
				elif detect_fall(last, sample, wewb):
					print_addr_op(addr, addr_format, 'write', samplenum, triggerpos, rate)
					state = 'begin'
		last = sample
