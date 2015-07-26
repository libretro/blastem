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

for path in glob('ztests/*/*.bin'):
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
		b = subprocess.check_output(['./ztestrun', path])
		try:
			m = subprocess.check_output(['gxz80/gxzrun', path])
			#_,_,b = b.partition('\n')
			if b != m:
				print '-----------------------------'
				print 'Mismatch in ' + path
				print 'blastem output:'
				print b
				print 'gxz80 output:'
				print m
				print '-----------------------------'
			else:
				print path, 'passed'
		except subprocess.CalledProcessError as e:
			print '-----------------------------'
			print 'gxz80 exited with code', e.returncode, 'for test', path
			print 'blastem output:'
			print b
			print '-----------------------------'
	except subprocess.CalledProcessError as e:
		print '-----------------------------'
		print 'blastem exited with code', e.returncode, 'for test', path
		print '-----------------------------'

