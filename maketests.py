#!/usr/bin/env python
from glob import glob
import subprocess
from sys import exit

sources = set()
for path in glob('generated_tests/*/*.s68'):
	sources.add(path)

bins = set()
for path in glob('generated_tests/*/*.bin'):
	bins.add(path)

for path in sources:
	binpath = path.replace('.s68', '.bin')
	if not binpath in bins:
		print binpath
		res = subprocess.call(['vasmm68k_mot', '-Fbin', '-m68000', '-no-opt', '-spaces', '-o', binpath, path])
		if res != 0:
			print 'vasmm68k_mot returned non-zero status code', res
			exit(1)

