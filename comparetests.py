#!/usr/bin/env python
from glob import glob
import subprocess
from sys import exit,argv

prefixes = []
skip = set()
for i in range(1, len(argv)):
	if '.' in argv[i]:
		f = open(argv[i])
		for line in f:
			parts = line.split()
			for part in parts:
				if part.endswith('.bin'):
					skip.add(part)
		f.close()
		print 'Skipping',len(skip),'entries from previous report.'
	else:
		prefixes.append(argv[i])

def print_mismatch(path, b, m):
	blines = b.split('\n')
	mlines = m.split('\n')
	if len(blines) != len(mlines):
		print '-----------------------------'
		print 'Unknown mismatch in', path
		print 'blastem output:'
		print b
		print 'musashi output:'
		print m
		print '-----------------------------'
		return
	prevline = ''
	differences = []
	flagmismatch = False
	regmismatch = False
	for i in xrange(0, len(blines)):
		if blines[i] != mlines[i]:
			if prevline == 'XNZVC':
				differences.append((prevline, prevline))
				flagmismatch = True
			else:
				regmismatch = True
			differences.append((blines[i], mlines[i]))
		prevline = blines[i]
	if flagmismatch and regmismatch:
		mtype = 'General'
	elif flagmismatch:
		mtype = 'Flag'
	elif regmismatch:
		mtype = 'Register'
	else:
		mtype = 'Unknown'
	print '-----------------------------'
	print mtype, 'mismatch in', path
	for i in xrange(0, 2):
		print 'musashi' if i else 'blastem', 'output:'
		for diff in differences:
			print diff[i]
	print '-----------------------------'



for path in glob('generated_tests/*/*.bin'):
	if path in skip:
		continue
	if prefixes:
		good = False
		fname = path.split('/')[-1]
		for prefix in prefixes:
			if fname.startswith(prefix):
				good = True
				break
		if not good:
			continue
	try:
		b = subprocess.check_output(['./trans', path])
		try:
			m = subprocess.check_output(['musashi/mustrans', path])
			#_,_,b = b.partition('\n')
			if b != m:
				print_mismatch(path, b, m)

			else:
				print path, 'passed'
		except subprocess.CalledProcessError as e:
			print '-----------------------------'
			print 'musashi exited with code', e.returncode, 'for test', path
			print 'blastem output:'
			print b
			print '-----------------------------'
	except subprocess.CalledProcessError as e:
		print '-----------------------------'
		print 'blastem exited with code', e.returncode, 'for test', path
		print '-----------------------------'

