#!/usr/bin/env python

def split_fields(line):
	parts = []
	while line:
		field,_,line = line.partition('\t')
		parts.append(field.strip())
		while line.startswith('\t'):
			line = line[1:]
	return parts

class Program(object):
	def __init__(self, instruction):
		self.avail_dregs = {0,1,2,3,4,5,6,7}
		self.avail_aregs = {0,1,2,3,4,5,6,7}
		instruction.consume_regs(self)
		self.inst = instruction
	
	def dirname(self):
		return self.inst.name + '_' + self.inst.size
	def name(self):
		return str(self.inst).replace('.', '_').replace('#', '_').replace(',', '_').replace(' ', '_').replace('(', '[').replace(')', ']')
	
	def write_rom_test(self, outfile):
		outfile.write('\tdc.l $0, start\n')
		needdivzero = self.inst.name.startswith('div')
		needchk = self.inst.name.startswith('chk')
		for i in xrange(0x8, 0x100, 0x4):
			if needdivzero and i == 0x14:
				outfile.write('\tdc.l div_zero_handler\n')
			elif needchk and i == 0x18:
				outfile.write('\tdc.l chk_handler\n')
			else:
				outfile.write('\tdc.l empty_handler\n')
		outfile.write('\tdc.b "SEGA"\nempty_handler:\n\trte\n')
		if needdivzero:
			outfile.write('div_zero_handler:\n')
			div_zero_count = self.get_dreg()
			outfile.write('\taddq #1, ' + str(div_zero_count) + '\n')
			outfile.write('\trte\n')
		if needchk:
			outfile.write('chk_handler:\n')
			chk_count = self.get_dreg()
			outfile.write('\taddq #1, ' + str(chk_count) + '\n')
			outfile.write('\trte\n')
		outfile.write('start:\n\tmove #0, CCR\n')
		if needdivzero:
			outfile.write('\tmoveq #0, ' + str(div_zero_count) + '\n')
		already = {}
		self.inst.write_init(outfile, already)
		if 'label' in already:
			outfile.write('lbl_' + str(already['label']) + ':\n')
		outfile.write('\t'+str(self.inst)+'\n')
		outfile.write('\t'+self.inst.save_result(self.get_dreg(), True) + '\n')
		save_ccr = self.get_dreg()
		outfile.write('\tmove SR, ' + str(save_ccr) + '\n')
		outfile.write('\tmove #$1F, CCR\n')
		self.inst.invalidate_dest(already)
		self.inst.write_init(outfile, already)
		if 'label' in already:
			outfile.write('lbl_' + str(already['label']) + ':\n')
		outfile.write('\t'+str(self.inst)+'\n')
		outfile.write('\t'+self.inst.save_result(self.get_dreg(), False) + '\n')
		outfile.write('\treset\nforever:\n\tbra.s forever\n')
	
	def consume_dreg(self, num):
		self.avail_dregs.discard(num)
	
	def consume_areg(self, num):
		self.avail_aregs.discard(num)
	
	def get_dreg(self):
		return Register('d', self.avail_dregs.pop())

class Dummy(object):
	def __str__(self):
		return ''
	def write_init(self, outfile, size, already):
		pass
	def consume_regs(self, program):
		pass

dummy_op = Dummy()

class Register(object):
	def __init__(self, kind, num):
		self.kind = kind
		self.num = num
	
	def __str__(self):
		if self.kind == 'd' or self.kind == 'a':
			return self.kind + str(self.num)
		return self.kind
	
	def write_init(self, outfile, size, already):
		if not str(self) in already:
			minv,maxv = get_size_range(size)
			val = randint(minv,maxv)
			already[str(self)] = val
			outfile.write('\tmove.'+size+' #'+str(val)+', ' + str(self) + '\n')
	
	def consume_regs(self, program):
		if self.kind == 'd':
			program.consume_dreg(self.num)
		elif self.kind == 'a':
			program.consume_areg(self.num)

def valid_ram_address(address, size='b'):
	return address >= 0xE00000 and address <= 0xFFFFFFFC and (address & 0xE00000) == 0xE00000 and (size == 'b' or not address & 1)

def random_ram_address(mina=0xE00000, maxa=0xFFFFFFFC):
	return randint(mina/2, maxa/2)*2 | 0xE00000

class Indexed(object):
	def __init__(self, base, index, index_size, disp):
		self.base = base
		self.index = index
		self.index_size = index_size
		self.disp = disp
	
	def write_init(self, outfile, size, already):
		if self.base.kind == 'pc':
			if str(self.index) in already:
				index = already[str(self.index)]
				if self.index_size == 'w':
					index = index & 0xFFFF
					#sign extend index
					if index & 0x8000:
						index -= 65536
				if index > -1024:
					index = already[str(self.index)] = 2 * randint(-16384, -512)
					outfile.write('\tmove.l #' + str(index) + ', ' + str(self.index) + '\n')
			else:
				index = already[str(self.index)] = 2 * randint(-16384, -512)
				outfile.write('\tmove.l #' + str(index) + ', ' + str(self.index) + '\n')
			num = already.get('label', 0)+1
			already['label'] = num
			if (already[str(self.index)] + self.disp) & 1:
				if self.disp > 0:
					self.disp -= 1
				else:
					self.disp += 1
			address = 'lbl_' + str(num) + ' + 2 + ' + str(self.disp) + ' + ' + str(index)
		else:
			if self.base == self.index:
				if str(self.base) in already:
					if not valid_ram_address(already[str(self.base)]*2):
						del already[str(self.base)]
						self.write_init(outfile, size, already)
						return
					else:
						base = index = already[str(self.base)]
				else:
					base = index = already[str(self.base)] = random_ram_address()/2
					outfile.write('\tmove.l #' + str(base) + ', ' + str(self.base) + '\n')
			else:
				if str(self.base) in already:
					if not valid_ram_address(already[str(self.base)]):
						del already[str(self.base)]
						self.write_init(outfile, size, already)
						return
					else:
						base = already[str(self.base)]
				else:
					base = already[str(self.base)] = random_ram_address()
					outfile.write('\tmove.l #' + str(base) + ', ' + str(self.base) + '\n')
				if str(self.index) in already:
					index = already[str(self.index)]
					if self.index_size == 'w':
						index = index & 0xFFFF
						#sign extend index
						if index & 0x8000:
							index -= 65536
					if not valid_ram_address(base + index):
						index = already[str(self.index)] = randint(-64, 63)
						outfile.write('\tmove.l #' + str(index) + ', ' + str(self.index) + '\n')
				else:
					index = already[str(self.index)] = randint(-64, 63)
					outfile.write('\tmove.l #' + str(index) + ', ' + str(self.index) + '\n')
			address = base + index + self.disp
			if (address & 0xFFFFFF) < 0xE00000:
				if (address & 0xFFFFFF) < 128:
					self.disp -= (address & 0xFFFFFF)
				else:
					self.disp += 0xE00000-(address & 0xFFFFFF)
				if self.disp > 127:
					self.disp = 127
				elif self.disp < -128:
					self.disp = -128
				address = base + index + self.disp
			elif (address & 0xFFFFFF) > 0xFFFFFC:
				self.disp -= (address & 0xFFFFFF) - 0xFFFFFC
				if self.disp > 127:
					self.disp = 127
				elif self.disp < -128:
					self.disp = -128
				address = base + index + self.disp
			if size != 'b' and address & 1:
				self.disp = self.disp ^ 1
				address = base + index + self.disp
		minv,maxv = get_size_range(size)
		outfile.write('\tmove.' + size + ' #' + str(randint(minv, maxv)) + ', (' + str(address) + ').l\n')
	
	def __str__(self):
		return '(' + str(self.disp) + ', ' + str(self.base) + ', ' + str(self.index) + '.' + self.index_size + ')'
	
	def consume_regs(self, program):
		self.base.consume_regs(program)
		self.index.consume_regs(program)

class Displacement(object):
	def __init__(self, base, disp):
		self.base = base
		if disp & 1:
			disp += 1
		self.disp = disp 
	
	def write_init(self, outfile, size, already):
		if self.base.kind == 'pc':
			num = already.get('label', 0)+1
			already['label'] = num
			address = 'lbl_' + str(num) + ' + 2 + ' + str(self.disp)
		else:
			if str(self.base) in already:
				if not valid_ram_address(already[str(self.base)]):
					del already[str(self.base)]
					self.write_init(outfile, size, already)
					return
				else:
					base = already[str(self.base)]
			else:
				base = already[str(self.base)] = random_ram_address()
				outfile.write('\tmove.l #' + str(base) + ', ' + str(self.base) + '\n')
			address = base + self.disp
			if (address & 0xFFFFFF) < 0xE00000:
				if (address & 0xFFFFFF) < 0x10000:
					self.disp -= (address & 0xFFFFFF)
				else:
					self.disp += 0xE00000-(address & 0xFFFFFF)
				address = base + self.disp
			elif (address & 0xFFFFFF) > 0xFFFFFC:
				self.disp -= (address & 0xFFFFFF) - 0xFFFFFC
				address = base + self.disp
			if size != 'b' and address & 1:
				self.disp = self.disp ^ 1
				address = base + self.disp
		minv,maxv = get_size_range(size)
		outfile.write('\tmove.' + size + ' #' + str(randint(minv, maxv)) + ', (' + str(address) + ').l\n')
	
	def __str__(self):
		return '(' + str(self.disp) + ', ' + str(self.base) + ')'
	
	def consume_regs(self, program):
		self.base.consume_regs(program)
	
class Indirect(object):
	def __init__(self, reg):
		self.reg = reg
	
	def __str__(self):
		return '(' + str(self.reg) + ')'
	
	def write_init(self, outfile, size, already):
		if str(self.reg) in already:
			if not valid_ram_address(already[str(self.reg)], size):
				del already[str(self.reg)]
				self.write_init(outfile, size, already)
				return
			else:
				address = already[str(self.reg)]
		else:
			address = random_ram_address()
			if size != 'b':
				address = address & 0xFFFFFFFE
			outfile.write('\tmove.l #' + str(address) + ', ' + str(self.reg) + '\n')
			already[str(self.reg)] = address
		minv,maxv = get_size_range(size)
		outfile.write('\tmove.' + size + ' #' + str(randint(minv, maxv)) + ', (' + str(address) + ').l\n')
	
	def consume_regs(self, program):
		self.reg.consume_regs(program)

class Increment(object):
	def __init__(self, reg):
		self.reg = reg
	
	def __str__(self):
		return '(' + str(self.reg) + ')+'
	
	def write_init(self, outfile, size, already):
		if str(self.reg) in already:
			if not valid_ram_address(already[str(self.reg)], size):
				del already[str(self.reg)]
				self.write_init(outfile, size, already)
				return
			else:
				address = already[str(self.reg)]
		else:
			address = random_ram_address()
			if size != 'b':
				address = address & 0xFFFFFFFE
			outfile.write('\tmove.l #' + str(address) + ', ' + str(self.reg) + '\n')
			already[str(self.reg)] = address
		minv,maxv = get_size_range(size)
		outfile.write('\tmove.' + size + ' #' + str(randint(minv, maxv)) + ', (' + str(address) + ').l\n')
	
	def consume_regs(self, program):
		self.reg.consume_regs(program)

class Decrement(object):
	def __init__(self, reg):
		self.reg = reg
	
	def __str__(self):
		return '-(' + str(self.reg) + ')'
	
	def write_init(self, outfile, size, already):
		if str(self.reg) in already:
			if not valid_ram_address(already[str(self.reg)]- 4 if size == 'l' else 2 if size == 'w' else 1, size):
				del already[str(self.reg)]
				self.write_init(outfile, size, already)
				return
			else:
				address = already[str(self.reg)]
		else:
			address = random_ram_address(mina=0xE00004)
			if size != 'b':
				address = address & 0xFFFFFFFE
			outfile.write('\tmove.l #' + str(address) + ', ' + str(self.reg) + '\n')
			already[str(self.reg)] = address
		minv,maxv = get_size_range(size)
		outfile.write('\tmove.' + size + ' #' + str(randint(minv, maxv)) + ', (' + str(address) + ').l\n')
	
	def consume_regs(self, program):
		self.reg.consume_regs(program)

class Absolute(object):
	def __init__(self, address, size):
		self.address = address
		self.size = size
	
	def __str__(self):
		return '(' + str(self.address) + ').' + self.size
	
	def write_init(self, outfile, size, already):
		minv,maxv = get_size_range(size)
		outfile.write('\tmove.' + size + ' #' + str(randint(minv, maxv)) + ', '+str(self)+'\n')
	
	def consume_regs(self, program):
		pass

class Immediate(object):
	def __init__(self, value):
		self.value = value
	
	def __str__(self):
		return '#' + str(self.value)
	
	def write_init(self, outfile, size, already):
		pass
	
	def consume_regs(self, program):
		pass
		
all_dregs = [Register('d', i) for i in range(0, 8)]
all_aregs = [Register('a', i) for i in range(0, 8)]
all_indirect = [Indirect(reg) for reg in all_aregs]
all_predec = [Decrement(reg) for reg in all_aregs]
all_postinc = [Increment(reg) for reg in all_aregs]
from random import randint
def all_indexed():
	return [Indexed(base, index, index_size, randint(-128, 127)) for base in all_aregs for index in all_dregs + all_aregs for index_size in ('w','l')]

def all_disp():
	return [Displacement(base, randint(-32768, 32767)) for base in all_aregs]

def rand_pc_disp():
	return [Displacement(Register('pc', 0), randint(-32768, -1024)) for x in xrange(0, 8)]

def all_pc_indexed():
	return [Indexed(Register('pc', 0), index, index_size, randint(-128, 127)) for index in all_dregs + all_aregs for index_size in ('w','l')]

def rand_abs_short():
	return [Absolute(random_ram_address(0xFFFF8000), 'w') for x in xrange(0, 8)]

def rand_abs_long():
	return [Absolute(random_ram_address(), 'l') for x in xrange(0, 8)]

def get_size_range(size):
	if size == 'b':
		return (-128, 127)
	elif size == 'w':
		return (-32768, 32767)
	else:
		return (-2147483648, 2147483647)

def rand_immediate(size):
	minv,maxv = get_size_range(size)
	
	return [Immediate(randint(minv, maxv)) for x in xrange(0,8)]

def get_variations(mode, size):
	mapping = {
		'd':all_dregs,
		'a':all_aregs,
		'(a)':all_indirect,
		'-(a)':all_predec,
		'(a)+':all_postinc,
		'(n,a)':all_disp,
		'(n,a,x)':all_indexed,
		'(n,pc)':rand_pc_disp,
		'(n,pc,x)':all_pc_indexed,
		'(n).w':rand_abs_short,
		'(n).l':rand_abs_long
	}
	if mode in mapping:
		ret = mapping[mode]
		if type(ret) != list:
			ret = ret()
		return ret
	elif mode == '#n':
		return rand_immediate(size)
	elif mode.startswith('#(') and mode.endswith(')'):
		inner = mode[2:-1]
		start,sep,end = inner.rpartition('-')
		start,end = int(start),int(end)
		if end-start > 16:
			return [Immediate(randint(start, end)) for x in range(0,8)]
		else:
			return [Immediate(num) for num in range(start, end+1)]
	else:
		print "Don't know what to do with source type", mode
		return None
		
class Inst2Op(object):
	def __init__(self, name, size, src, dst):
		self.name = name
		self.size = size
		self.src = src
		self.dst = dst
	
	def __str__(self):
		return self.name + '.' + self.size + ' ' + str(self.src) + ', ' + str(self.dst)
	
	def write_init(self, outfile, already):
		self.src.write_init(outfile, self.size, already)
		self.dst.write_init(outfile, self.size, already)
	
	def invalidate_dest(self, already):
		if type(self.dst) == Register:
			del already[str(self.dst)]
	
	def save_result(self, reg, always):
		if always or type(self.dst) != Register:
			if type(self.dst) == Decrement:
				src = Increment(self.dst.reg)
			elif type(self.dst) == Increment:
				src = Decrement(self.dst.reg)
			else:
				src = self.dst
			return 'move.' + self.size + ' ' + str(src) + ', ' + str(reg)
		else:
			return ''
	
	def consume_regs(self, program):
		self.src.consume_regs(program)
		self.dst.consume_regs(program)

class Inst1Op(Inst2Op):
	def __init__(self, name, size, dst):
		super(Inst1Op, self).__init__(name, size, dummy_op, dst)
	
	def __str__(self):
		return self.name + '.' + self.size + ' ' + str(self.dst)

class Entry(object):
	def __init__(self, line):
		fields = split_fields(line)
		self.name = fields[0]
		sizes = fields[1]
		sources = fields[2].split(';')
		if len(fields) > 3:
			dests = fields[3].split(';')
		else:
			dests = None
		combos = []
		for size in sizes:
			for source in sources:
				if size != 'b' or source != 'a':
					if dests:
						for dest in dests:
							if size != 'b' or dest != 'a':
								combos.append((size, source, dest))
					else:
						combos.append((size, None, source))
		self.cases = combos
		
	def programs(self):
		res = []
		for (size, src, dst) in self.cases:
			dests = get_variations(dst, size)
			if src:
				sources = get_variations(src, size)
				for source in sources:
					for dest in dests:
						res.append(Program(Inst2Op(self.name, size, source, dest)))
			else:
				for dest in dests:
					res.append(Program(Inst1Op(self.name, size, dest)))
		return res
		
def process_entries(f):
	entries = []
	for line in f:
		if not line.startswith('Name') and not line.startswith('#') and len(line.strip()) > 0:
			entries.append(Entry(line))
	return entries

from os import path, mkdir
def main(args):
	entries = process_entries(open('testcases.txt'))
	for entry in entries:
		programs = entry.programs()
		for program in programs:
			dname = program.dirname()
			if not path.exists('generated_tests/' + dname):
				mkdir('generated_tests/' + dname)
			f = open('generated_tests/' + dname + '/' + program.name() + '.s68', 'w')
			program.write_rom_test(f)
			f.close()
	
if __name__ == '__main__':
	import sys
	main(sys.argv)

