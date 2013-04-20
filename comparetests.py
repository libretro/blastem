#!/usr/bin/env python
from glob import glob
import subprocess
from sys import exit,argv

prefixes = []
for i in range(1, len(argv)):
	prefixes.append(argv[i])

for path in glob('generated_tests/*.bin'):
	if prefixes:
		good = False
		for prefix in prefixes:
			if path.startswith(prefix):
				good = True
				break
		if not good:
			continue
	try:
		b = subprocess.check_output(['./blastem', path, '-v'])
		try:
			m = subprocess.check_output(['musashi/mustrans', path])
			#_,_,b = b.partition('\n')
			if b != m:
				print '-----------------------------'
				print 'Mismatch in ' + path
				print 'blastem output:'
				print b
				print 'musashi output:'
				print m
				print '-----------------------------'
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

