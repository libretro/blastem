#!/usr/bin/env python
from glob import glob
import subprocess
from sys import exit

for path in glob('generated_tests/*.bin'):
	try:
		b = subprocess.check_output(['./blastem', path])
		try:
			m = subprocess.check_output(['musashi/mustrans', path])
			_,_,b = b.partition('\n')
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

