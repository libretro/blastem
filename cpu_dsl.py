#!/usr/bin/env python3


class Block:
	def addOp(self, op):
		pass
	
	def processLine(self, parts):
		if parts[0] == 'switch':
			o = Switch(self, parts[1])
			self.addOp(o)
			return o
		elif parts[0] == 'if':
			o = If(self, parts[1])
			self.addOp(o)
			return o
		elif parts[0] == 'end':
			raise Exception('end is only allowed inside a switch or if block')
		else:
			self.addOp(NormalOp(parts))
		return self
		
	def resolveLocal(self, name):
		return None
			
class ChildBlock(Block):
	def processLine(self, parts):
		if parts[0] == 'end':
			return self.parent
		return super().processLine(parts)

#Represents an instruction of the emulated CPU
class Instruction(Block):
	def __init__(self, value, fields, name):
		self.value = value
		self.fields = fields
		self.name = name
		self.implementation = []
		self.locals = {}
		self.regValues = {}
		self.varyingBits = 0
		self.invalidFieldValues = {}
		self.newLocals = []
		for field in fields:
			self.varyingBits += fields[field][1]
	
	def addOp(self, op):
		if op.op == 'local':
			name = op.params[0]
			size = op.params[1]
			self.locals[name] = size
		elif op.op == 'invalid':
			name = op.params[0]
			value = int(op.params[1])
			self.invalidFieldValues.setdefault(name, set()).add(value)
		else:
			self.implementation.append(op)
			
	def resolveLocal(self, name):
		if name in self.locals:
			return name
		return None
	
	def addLocal(self, name, size):
		self.locals[name] = size
		self.newLocals.append(name)
		
	def localSize(self, name):
		return self.locals.get(name)
			
	def __lt__(self, other):
		if isinstance(other, Instruction):
			if self.varyingBits != other.varyingBits:
				return self.varyingBits < other.varyingBits
			return self.value < other.value
		else:
			return NotImplemented
			
	def allValues(self):
		values = []
		for i in range(0, 1 << self.varyingBits):
			iword = self.value
			doIt = True
			for field in self.fields:
				shift,bits = self.fields[field]
				val = i & ((1 << bits) - 1)
				if field in self.invalidFieldValues and val in self.invalidFieldValues[field]:
					doIt = False
					break
				i >>= bits
				iword |= val << shift
			if doIt:
				values.append(iword)
		return values
		
	def getFieldVals(self, value):
		fieldVals = {}
		fieldBits = {}
		for field in self.fields:
			shift,bits = self.fields[field]
			val = (value >> shift) & ((1 << bits) - 1)
			fieldVals[field] = val
			fieldBits[field] = bits
		return (fieldVals, fieldBits)
	
	def generateName(self, value):
		fieldVals,fieldBits = self.getFieldVals(value)
		names = list(fieldVals.keys())
		names.sort()
		funName = self.name
		for name in names:
			funName += '_{0}_{1:0>{2}}'.format(name, bin(fieldVals[name])[2:], fieldBits[name])
		return funName
		
	def generateBody(self, value, prog, otype):
		output = []
		prog.meta = {}
		prog.pushScope(self)
		self.regValues = {}
		for var in self.locals:
			output.append('\n\tuint{sz}_t {name};'.format(sz=self.locals[var], name=var))
		self.newLocals = []
		fieldVals,_ = self.getFieldVals(value)
		for op in self.implementation:
			op.generate(prog, self, fieldVals, output, otype)
		begin = '\nvoid ' + self.generateName(value) + '(' + prog.context_type + ' *context)\n{'
		if prog.needFlagCoalesce:
			begin += prog.flags.coalesceFlags(prog, otype)
		if prog.needFlagDisperse:
			output.append(prog.flags.disperseFlags(prog, otype))
		for var in self.newLocals:
			begin += '\n\tuint{sz}_t {name};'.format(sz=self.locals[var], name=var)
		prog.popScope()
		return begin + ''.join(output) + '\n}'
		
	def __str__(self):
		pieces = [self.name + ' ' + hex(self.value) + ' ' + str(self.fields)]
		for name in self.locals:
			pieces.append('\n\tlocal {0} {1}'.format(name, self.locals[name]))
		for op in self.implementation:
			pieces.append(str(op))
		return ''.join(pieces)
	
#Represents the definition of a helper function
class SubRoutine(Block):
	def __init__(self, name):
		self.name = name
		self.implementation = []
		self.args = []
		self.arg_map = {}
		self.locals = {}
		self.regValues = {}
	
	def addOp(self, op):
		if op.op == 'arg':
			name = op.params[0]
			size = op.params[1]
			self.arg_map[name] = len(self.args)
			self.args.append((name, size))
		elif op.op == 'local':
			name = op.params[0]
			size = op.params[1]
			self.locals[name] = size
		else:
			self.implementation.append(op)
			
	def resolveLocal(self, name):
		if name in self.locals:
			return self.name + '_' + name
		return None
	
	def addLocal(self, name, size):
		self.locals[name] = size
	
	def localSize(self, name):
		return self.locals.get(name)
			
	def inline(self, prog, params, output, otype, parent):
		if len(params) != len(self.args):
			raise Exception('{0} expects {1} arguments, but was called with {2}'.format(self.name, len(self.args), len(params)))
		argValues = {}
		if parent:
			self.regValues = parent.regValues
		prog.pushScope(self)
		i = 0
		for name,size in self.args:
			argValues[name] = params[i]
			i += 1
		for name in self.locals:
			size = self.locals[name]
			output.append('\n\tuint{size}_t {sub}_{local};'.format(size=size, sub=self.name, local=name))
		for op in self.implementation:
			op.generate(prog, self, argValues, output, otype)
		prog.popScope()
		
	def __str__(self):
		pieces = [self.name]
		for name,size in self.args:
			pieces.append('\n\targ {0} {1}'.format(name, size))
		for name in self.locals:
			pieces.append('\n\tlocal {0} {1}'.format(name, self.locals[name]))
		for op in self.implementation:
			pieces.append(str(op))
		return ''.join(pieces)
	
class Op:
	def __init__(self, evalFun = None):
		self.evalFun = evalFun
		self.impls = {}
		self.outOp = ()
	def cBinaryOperator(self, op):
		def _impl(prog, params):
			if op == '-':
				a = params[1]
				b = params[0]
			else:
				a = params[0]
				b = params[1]
			return '\n\t{dst} = {a} {op} {b};'.format(
				dst = params[2], a = a, b = b, op = op
			)
		self.impls['c'] = _impl
		self.outOp = (2,)
		return self
	def cUnaryOperator(self, op):
		def _impl(prog, params):
			return '\n\t{dst} = {op}{a};'.format(
				dst = params[1], a = params[0], op = op
			)
		self.impls['c'] = _impl
		self.outOp = (1,)
		return self
	def addImplementation(self, lang, outOp, impl):
		self.impls[lang] = impl
		if not outOp is None:
			if type(outOp) is tuple:
				self.outOp = outOp
			else:
				self.outOp = (outOp,)
		return self
	def evaluate(self, params):
		return self.evalFun(*params)
	def canEval(self):
		return not self.evalFun is None
	def numArgs(self):
		return self.evalFun.__code__.co_argcount
	def generate(self, otype, prog, params, rawParams):
		if self.impls[otype].__code__.co_argcount == 2:
			return self.impls[otype](prog, params)
		else:
			return self.impls[otype](prog, params, rawParams)
		
		
def _xchgCImpl(prog, params, rawParams):
	size = prog.paramSize(rawParams[0])
	decl,name = prog.getTemp(size)
	return decl + '\n\t{tmp} = {a};\n\t{a} = {b};\n\t{b} = {tmp};'.format(a = params[0], b = params[1], tmp = name)

def _dispatchCImpl(prog, params):
	if len(params) == 1:
		table = 'main'
	else:
		table = params[1]
	return '\n\timpl_{tbl}[{op}](context);'.format(tbl = table, op = params[0])

def _updateFlagsCImpl(prog, params, rawParams):
	i = 0
	last = ''
	autoUpdate = set()
	explicit = {}
	for c in params[0]:
		if c.isdigit():
			if last.isalpha():
				num = int(c)
				if num > 1:
					raise Exception(c + ' is not a valid digit for update_flags')
				explicit[last] = num
				last = c
			else:
				raise Exception('Digit must follow flag letter in update_flags')
		else:
			if last.isalpha():
				autoUpdate.add(last)
			last = c
	if last.isalpha():
		autoUpdate.add(last)
	output = []
	#TODO: handle autoUpdate flags
	for flag in autoUpdate:
		calc = prog.flags.flagCalc[flag]
		calc,_,resultBit = calc.partition('-')
		lastDst = prog.resolveParam(prog.lastDst, None, {})
		storage = prog.flags.getStorage(flag)
		if calc == 'bit' or calc == 'sign':
			if calc == 'sign':
				resultBit = prog.paramSize(prog.lastDst) - 1
			else:
				resultBit = int(resultBit)
			if type(storage) is tuple:
				reg,storageBit = storage
				reg = prog.resolveParam(reg, None, {})
				if storageBit == resultBit:
					#TODO: optimize this case
					output.append('\n\t{reg} = ({reg} & ~{mask}U) | ({res} & {mask}U);'.format(
						reg = reg, mask = 1 << resultBit, res = lastDst
					))
				else:
					if resultBit > storageBit:
						op = '>>'
						shift = resultBit - storageBit
					else:
						op = '<<'
						shift = storageBit - resultBit
					output.append('\n\t{reg} = ({reg} & ~{mask}U) | ({res} {op} {shift}U & {mask}U);'.format(
						reg = reg, mask = 1 << storageBit, res = lastDst, op = op, shift = shift
					))
			else:
				reg = prog.resolveParam(storage, None, {})
				output.append('\n\t{reg} = {res} & {mask}U;'.format(reg=reg, res=lastDst, mask = 1 << resultBit))
		elif calc == 'zero':
			if type(storage) is tuple:
				reg,storageBit = storage
				reg = prog.resolveParam(reg, None, {})
				output.append('\n\t{reg} = {res} ? ({reg} & {mask}U) : ({reg} | {bit}U);'.format(
					reg = reg, mask = ~(1 << storageBit), res = lastDst, bit = 1 << storageBit
				))
			elif prog.paramSize(prog.lastDst) > prog.paramSize(storage):
				reg = prog.resolveParam(storage, None, {})
				output.append('\n\t{reg} = {res} != 0;'.format(
					reg = reg, res = lastDst
				))
			else:
				reg = prog.resolveParam(storage, None, {})
				output.append('\n\t{reg} = {res};'.format(reg = reg, res = lastDst))
		elif calc == 'half-carry':
			pass
		elif calc == 'carry':
			pass
		elif calc == 'overflow':
			pass
		elif calc == 'parity':
			pass
	#TODO: combine explicit flags targeting the same storage location
	for flag in explicit:
		location = prog.flags.getStorage(flag)
		if type(location) is tuple:
			reg,bit = location
			reg = prog.resolveReg(reg, None, {})
			value = str(1 << bit)
			if explicit[flag]:
				operator = '|='
			else:
				operator = '&='
				value = '~' + value
			output.append('\n\t{reg} {op} {val};'.format(reg=reg, op=operator, val=value))
		else:
			reg = prog.resolveReg(location, None, {})
			output.append('\n\t{reg} = {val};'.format(reg=reg, val=explicit[flag]))
	return ''.join(output)
	
def _cmpCImpl(prog, params):
	size = prog.paramSize(params[1])
	tmpvar = 'cmp_tmp{sz}__'.format(sz=size)
	typename = ''
	scope = prog.getRootScope()
	if not scope.resolveLocal(tmpvar):
		scope.addLocal(tmpvar, size)
	prog.lastDst = tmpvar
	return '\n\t{var} = {b} - {a};'.format(var = tmpvar, a = params[0], b = params[1])

def _asrCImpl(prog, params, rawParams):
	shiftSize = prog.paramSize(rawParams[0])
	mask = 1 << (shiftSize - 1)
	return '\n\t{dst} = ({a} >> {b}) | ({a} & {mask});'.format(a = params[0], b = params[1], dst = params[2], mask = mask)
		
_opMap = {
	'mov': Op(lambda val: val).cUnaryOperator(''),
	'not': Op(lambda val: ~val).cUnaryOperator('~'),
	'lnot': Op(lambda val: 0 if val else 1).cUnaryOperator('!'),
	'neg': Op(lambda val: -val).cUnaryOperator('-'),
	'add': Op(lambda a, b: a + b).cBinaryOperator('+'),
	'sub': Op(lambda a, b: b - a).cBinaryOperator('-'),
	'lsl': Op(lambda a, b: a << b).cBinaryOperator('<<'),
	'lsr': Op(lambda a, b: a >> b).cBinaryOperator('>>'),
	'asr': Op(lambda a, b: a >> b).addImplementation('c', 2, _asrCImpl),
	'and': Op(lambda a, b: a & b).cBinaryOperator('&'),
	'or':  Op(lambda a, b: a | b).cBinaryOperator('|'),
	'xor': Op(lambda a, b: a ^ b).cBinaryOperator('^'),
	'abs': Op(lambda val: abs(val)).addImplementation(
		'c', 1, lambda prog, params: '\n\t{dst} = abs({src});'.format(dst=params[1], src=params[0])
	),
	'cmp': Op().addImplementation('c', None, _cmpCImpl),
	'ocall': Op().addImplementation('c', None, lambda prog, params: '\n\t{pre}{fun}({args});'.format(
		pre = prog.prefix, fun = params[0], args = ', '.join(['context'] + [str(p) for p in params[1:]])
	)),
	'cycles': Op().addImplementation('c', None,
		lambda prog, params: '\n\tcontext->cycles += context->opts->gen.clock_divider * {0};'.format(
			params[0]
		)
	),
	'addsize': Op(
		lambda a, b: b + (2 * a if a else 1)
	).addImplementation('c', 2, lambda prog, params: '\n\t{dst} = {val} + {sz} ? {sz} * 2 : 1;'.format(
		dst = params[2], sz = params[0], val = params[1]
	)),
	'decsize': Op(
		lambda a, b: b - (2 * a if a else 1)
	).addImplementation('c', 2, lambda prog, params: '\n\t{dst} = {val} - {sz} ? {sz} * 2 : 1;'.format(
		dst = params[2], sz = params[0], val = params[1]
	)),
	'xchg': Op().addImplementation('c', (0,1), _xchgCImpl),
	'dispatch': Op().addImplementation('c', None, _dispatchCImpl),
	'update_flags': Op().addImplementation('c', None, _updateFlagsCImpl)
}

#represents a simple DSL instruction
class NormalOp:
	def __init__(self, parts):
		self.op = parts[0]
		self.params = parts[1:]
		
	def generate(self, prog, parent, fieldVals, output, otype):
		procParams = []
		allParamsConst = True
		opDef = _opMap.get(self.op)
		for param in self.params:
			allowConst = (self.op in prog.subroutines or len(procParams) != len(self.params) - 1) and param in parent.regValues
			isDst = (not opDef is None) and len(procParams) in opDef.outOp
			param = prog.resolveParam(param, parent, fieldVals, allowConst, isDst)
			
			if (not type(param) is int) and len(procParams) != len(self.params) - 1:
				allParamsConst = False
			procParams.append(param)
			
		if self.op == 'meta':
			param,_,index = self.params[1].partition('.')
			if index:
				index = (parent.resolveLocal(index) or index)
				if index in fieldVals:
					index = str(fieldVals[index])
				param = param + '.' + index
			else:
				param = parent.resolveLocal(param) or param
				if param in fieldVals:
					param = fieldVals[index]
			prog.meta[self.params[0]] = param
		elif self.op == 'dis':
			#TODO: Disassembler
			pass
		elif not opDef is None:
			if opDef.canEval() and allParamsConst:
				#do constant folding
				if opDef.numArgs() >= len(procParams):
					raise Exception('Insufficient args for ' + self.op + ' (' + ', '.join(self.params) + ')')
				dst = self.params[opDef.numArgs()]
				result = opDef.evaluate(procParams[:opDef.numArgs()])
				while dst in prog.meta:
					dst = prog.meta[dst]
				maybeLocal = parent.resolveLocal(dst)
				if maybeLocal:
					dst = maybeLocal
				parent.regValues[dst] = result
				if prog.isReg(dst):
					output.append(_opMap['mov'].generate(otype, prog, procParams, self.params))
			else:
				output.append(opDef.generate(otype, prog, procParams, self.params))
		elif self.op in prog.subroutines:
			prog.subroutines[self.op].inline(prog, procParams, output, otype, parent)
		else:
			output.append('\n\t' + self.op + '(' + ', '.join([str(p) for p in procParams]) + ');')
		prog.lastOp = self
	
	def __str__(self):
		return '\n\t' + self.op + ' ' + ' '.join(self.params)
		
#represents a DSL switch construct
class Switch(ChildBlock):
	def __init__(self, parent, param):
		self.op = 'switch'
		self.parent = parent
		self.param = param
		self.cases = {}
		self.regValues = None
		self.current_locals = {}
		self.case_locals = {}
		self.current_case = None
		self.default = None
		self.default_locals = None
	
	def addOp(self, op):
		if op.op == 'case':
			val = int(op.params[0], 16) if op.params[0].startswith('0x') else int(op.params[0])
			self.cases[val] = self.current_case = []
			self.case_locals[val] = self.current_locals = {}
		elif op.op == 'default':
			self.default = self.current_case = []
			self.default_locals = self.current_locals = {}
		elif self.current_case == None:
			raise ion('Orphan instruction in switch')
		elif op.op == 'local':
			name = op.params[0]
			size = op.params[1]
			self.current_locals[name] = size
		else:
			self.current_case.append(op)
			
	def resolveLocal(self, name):
		if name in self.current_locals:
			return name
		return self.parent.resolveLocal(name)
	
	def addLocal(self, name, size):
		self.current_locals[name] = size
		
	def localSize(self, name):
		if name in self.current_locals:
			return self.current_locals[name]
		return self.parent.localSize(name)
			
	def generate(self, prog, parent, fieldVals, output, otype):
		prog.pushScope(self)
		param = prog.resolveParam(self.param, parent, fieldVals)
		if type(param) is int:
			self.regValues = self.parent.regValues
			if param in self.cases:
				self.current_locals = self.case_locals[param]
				output.append('\n\t{')
				for local in self.case_locals[param]:
					output.append('\n\tuint{0}_t {1};'.format(self.case_locals[param][local], local))
				for op in self.cases[param]:
					op.generate(prog, self, fieldVals, output, otype)
				output.append('\n\t}')
			elif self.default:
				self.current_locals = self.default_locals
				output.append('\n\t{')
				for local in self.default_locals:
					output.append('\n\tuint{0}_t {1};'.format(self.default[local], local))
				for op in self.default:
					op.generate(prog, self, fieldVals, output, otype)
				output.append('\n\t}')
		else:
			output.append('\n\tswitch(' + param + ')')
			output.append('\n\t{')
			for case in self.cases:
				self.current_locals = self.case_locals[case]
				self.regValues = dict(self.parent.regValues)
				output.append('\n\tcase {0}U: '.format(case) + '{')
				for local in self.case_locals[case]:
					output.append('\n\tuint{0}_t {1};'.format(self.case_locals[case][local], local))
				for op in self.cases[case]:
					op.generate(prog, self, fieldVals, output, otype)
				output.append('\n\tbreak;')
				output.append('\n\t}')
			if self.default:
				self.current_locals = self.default_locals
				self.regValues = dict(self.parent.regValues)
				output.append('\n\tdefault: {')
				for local in self.default_locals:
					output.append('\n\tuint{0}_t {1};'.format(self.default_locals[local], local))
				for op in self.default:
					op.generate(prog, self, fieldVals, output, otype)
			output.append('\n\t}')
		prog.popScope()
	
	def __str__(self):
		keys = self.cases.keys()
		keys.sort()
		lines = ['\n\tswitch']
		for case in keys:
			lines.append('\n\tcase {0}'.format(case))
			lines.append(''.join([str(op) for op in self.cases[case]]))
		lines.append('\n\tend')
		return ''.join(lines)

		
def _geuCImpl(prog, parent, fieldVals, output):
	if prog.lastOp.op == 'cmp':
		output.pop()
		params = [prog.resolveParam(p, parent, fieldVals) for p in prog.lastOp.params]
		return '\n\tif ({a} >= {b}) '.format(a=params[1], b = params[0]) + '{'
	else:
		raise ion(">=U not implemented in the general case yet")
	
_ifCmpImpl = {
	'c': {
		'>=U': _geuCImpl
	}
}
#represents a DSL conditional construct
class If(ChildBlock):
	def __init__(self, parent, cond):
		self.op = 'if'
		self.parent = parent
		self.cond = cond
		self.body = []
		self.elseBody = []
		self.curBody = self.body
		self.locals = {}
		self.elseLocals = {}
		self.curLocals = self.locals
		self.regValues = None
		
	def addOp(self, op):
		if op.op in ('case', 'arg'):
			raise Exception(self.op + ' is not allows inside an if block')
		if op.op == 'local':
			name = op.params[0]
			size = op.params[1]
			self.locals[name] = size
		elif op.op == 'else':
			self.curLocals = self.elseLocals
			self.curBody = self.elseBody
		else:
			self.curBody.append(op)
			
	def localSize(self, name):
		return self.curLocals.get(name)
		
	def resolveLocal(self, name):
		if name in self.locals:
			return name
		return self.parent.resolveLocal(name)
		
	def _genTrueBody(self, prog, fieldVals, output, otype):
		self.curLocals = self.locals
		for local in self.locals:
			output.append('\n\tuint{sz}_t {nm};'.format(sz=self.locals[local], nm=local))
		for op in self.body:
			op.generate(prog, self, fieldVals, output, otype)
			
	def _genFalseBody(self, prog, fieldVals, output, otype):
		self.curLocals = self.elseLocals
		for local in self.elseLocals:
			output.append('\n\tuint{sz}_t {nm};'.format(sz=self.elseLocals[local], nm=local))
		for op in self.elseBody:
			op.generate(prog, self, fieldVals, output, otype)
	
	def _genConstParam(self, param, prog, fieldVals, output, otype):
		if param:
			self._genTrueBody(prog, fieldVals, output, otype)
		else:
			self._genFalseBody(prog, fieldVals, output, otype)
			
	def generate(self, prog, parent, fieldVals, output, otype):
		self.regValues = parent.regValues
		try:
			self._genConstParam(prog.checkBool(self.cond), prog, fieldVals, output, otype)
		except Exception:
			if self.cond in _ifCmpImpl[otype]:
				output.append(_ifCmpImpl[otype][self.cond](prog, parent, fieldVals, output))
				self._genTrueBody(prog, fieldVals, output, otype)
				if self.elseBody:
					output.append('\n\t} else {')
					self._genFalseBody(prog, fieldVals, output, otype)
				output.append('\n\t}')
			else:
				cond = prog.resolveParam(self.cond, parent, fieldVals)
				if type(cond) is int:
					self._genConstParam(cond, prog, fieldVals, output, otype)
				else:
					output.append('\n\tif ({cond}) '.format(cond=cond) + '{')
					self._genTrueBody(prog, fieldVals, output, otype)
					if self.elseBody:
						output.append('\n\t} else {')
						self._genFalseBody(prog, fieldVals, output, otype)
					output.append('\n\t}')
						
	
	def __str__(self):
		lines = ['\n\tif']
		for op in self.body:
			lines.append(str(op))
		lines.append('\n\tend')
		return ''.join(lines)

class Registers:
	def __init__(self):
		self.regs = {}
		self.pointers = {}
		self.regArrays = {}
		self.regToArray = {}
	
	def addReg(self, name, size):
		self.regs[name] = size
		
	def addPointer(self, name, size):
		self.pointers[name] = size
	
	def addRegArray(self, name, size, regs):
		self.regArrays[name] = (size, regs)
		idx = 0
		if not type(regs) is int:
			for reg in regs:
				self.regs[reg] = size
				self.regToArray[reg] = (name, idx)
				idx += 1
	
	def isReg(self, name):
		return name in self.regs
	
	def isRegArray(self, name):
		return name in self.regArrays
		
	def isRegArrayMember(self, name):
		return name in self.regToArray
		
	def arrayMemberParent(self, name):
		return self.regToArray[name][0]
	
	def arrayMemberIndex(self, name):
		return self.regToArray[name][1]
	
	def arrayMemberName(self, array, index):
		if type(index) is int and not type(self.regArrays[array][1]) is int:
			return self.regArrays[array][1][index]
		else:
			return None
			
	def isNamedArray(self, array):
		return array in self.regArrays and type(self.regArrays[array][1]) is int
	
	def processLine(self, parts):
		if len(parts) == 3:
			self.addRegArray(parts[0], int(parts[1]), int(parts[2]))
		elif len(parts) > 2:
			self.addRegArray(parts[0], int(parts[1]), parts[2:])
		else:
			if parts[1].startswith('ptr'):
				self.addPointer(parts[0], int(parts[1][3:]))
			else:
				self.addReg(parts[0], int(parts[1]))
		return self

	def writeHeader(self, otype, hFile):
		fieldList = []
		for pointer in self.pointers:
			hFile.write('\n\tuint{sz}_t *{nm};'.format(nm=pointer, sz=self.pointers[pointer]))
		for reg in self.regs:
			if not self.isRegArrayMember(reg):
				fieldList.append((self.regs[reg], 1, reg))
		for arr in self.regArrays:
			size,regs = self.regArrays[arr]
			if not type(regs) is int:
				regs = len(regs)
			fieldList.append((size, regs, arr))
		fieldList.sort()
		fieldList.reverse()
		for size, count, name in fieldList:
			if count > 1:
				hFile.write('\n\tuint{sz}_t {nm}[{ct}];'.format(sz=size, nm=name, ct=count))
			else:
				hFile.write('\n\tuint{sz}_t {nm};'.format(sz=size, nm=name))
	
class Flags:
	def __init__(self):
		self.flagBits = {}
		self.flagCalc = {}
		self.flagStorage = {}
		self.flagReg = None
		self.maxBit = -1
	
	def processLine(self, parts):
		if parts[0] == 'register':
			self.flagReg = parts[1]
		else:
			flag,bit,calc,storage = parts
			bit,_,top = bit.partition('-')
			bit = int(bit)
			if top:
				top = int(bit)
				if top > self.maxBit:
					self.maxBit = top
				self.flagBits[flag] = (bit,top)
			else:
				if bit > self.maxBit:
					self.maxBit = bit
				self.flagBits[flag] = bit
			self.flagCalc[flag] = calc
			self.flagStorage[flag] = storage
		return self
	
	def getStorage(self, flag):
		if not flag in self.flagStorage:
			raise Exception('Undefined flag ' + flag)
		loc,_,bit = self.flagStorage[flag].partition('.')
		if bit:
			return (loc, int(bit))
		else:
			return loc 
	
	def disperseFlags(self, prog, otype):
		bitToFlag = [None] * (self.maxBit+1)
		src = prog.resolveReg(self.flagReg, None, {})
		output = []
		for flag in self.flagBits:
			bit = self.flagBits[flag]
			if type(bit) is tuple:
				bot,top = bit
				mask = ((1 << (top + 1 - bot)) - 1) << bot
				output.append('\n\t{dst} = {src} & mask;'.format(
					dst=prog.resolveReg(self.flagStorage[flag], None, {}), src=src, mask=mask
				))
			else:
				bitToFlag[self.flagBits[flag]] = flag		
		multi = {}
		for bit in range(len(bitToFlag)-1,-1,-1):
			flag = bitToFlag[bit]
			if not flag is None:
				field,_,dstbit = self.flagStorage[flag].partition('.')
				dst = prog.resolveReg(field, None, {})
				if dstbit:
					dstbit = int(dstbit)
					multi.setdefault(dst, []).append((dstbit, bit))
				else:
					output.append('\n\t{dst} = {src} & {mask};'.format(dst=dst, src=src, mask=(1 << bit)))
		for dst in multi:
			didClear = False
			direct = []
			for dstbit, bit in multi[dst]:
				if dstbit == bit:
					direct.append(bit)
				else:
					if not didClear:
						output.append('\n\t{dst} = 0;'.format(dst=dst))
						didClear = True
					if dstbit > bit:
						shift = '<<'
						diff = dstbit - bit
					else:
						shift = '>>'
						diff = bit - dstbit
					output.append('\n\t{dst} |= {src} {shift} {diff} & {mask};'.format(
						src=src, dst=dst, shift=shift, diff=diff, mask=(1 << dstbit)
					))
			if direct:
				if len(direct) == len(multi[dst]):
					output.append('\n\t{dst} = {src};'.format(dst=dst, src=src))
				else:
					mask = 0
					for bit in direct:
						mask = mask | (1 << bit)
					output.append('\n\t{dst} = {src} & {mask};'.format(dst=dst, src=src, mask=mask))
		return ''.join(output)
	
	def coalesceFlags(self, prog, otype):
		dst = prog.resolveReg(self.flagReg, None, {})
		output = ['\n\t{dst} = 0;'.format(dst=dst)]
		bitToFlag = [None] * (self.maxBit+1)
		for flag in self.flagBits:
			bit = self.flagBits[flag]
			if type(bit) is tuple:
				bot,_ = bit
				src = prog.resolveReg(self.flagStorage[flag], None, {})
				if bot:
					output.append('\n\t{dst} |= {src} << {shift};'.format(
						dst=dst, src = src, shift = bot
					))
				else:
					output.append('\n\t{dst} |= {src};'.format(
						dst=dst, src = src
					))
			else:
				bitToFlag[bit] = flag
		multi = {}
		for bit in range(len(bitToFlag)-1,-1,-1):
			flag = bitToFlag[bit]
			if not flag is None:
				field,_,srcbit = self.flagStorage[flag].partition('.')
				src = prog.resolveReg(field, None, {})
				if srcbit:
					srcbit = int(srcbit)
					multi.setdefault(src, []).append((srcbit,bit))
				else:
					output.append('\n\tif ({src}) {{\n\t\t{dst} |= 1 << {bit};\n\t}}'.format(
						dst=dst, src=src, bit=bit
					))
		for src in multi:
			direct = 0
			for srcbit, dstbit in multi[src]:
				if srcbit == dstbit:
					direct = direct | (1 << srcbit)
				else:
					output.append('\n\tif ({src} & (1 << {srcbit})) {{\n\t\t{dst} |= 1 << {dstbit};\n\t}}'.format(
						src=src, dst=dst, srcbit=srcbit, dstbit=dstbit
					))
			if direct:
				output.append('\n\t{dst} |= {src} & {mask}'.format(
					dst=dst, src=src, mask=direct
				))
		return ''.join(output)
		
		
class Program:
	def __init__(self, regs, instructions, subs, info, flags):
		self.regs = regs
		self.instructions = instructions
		self.subroutines = subs
		self.meta = {}
		self.booleans = {}
		self.prefix = info.get('prefix', [''])[0]
		self.opsize = int(info.get('opcode_size', ['8'])[0])
		self.extra_tables = info.get('extra_tables', [])
		self.context_type = self.prefix + 'context'
		self.body = info.get('body', [None])[0]
		self.includes = info.get('include', [])
		self.flags = flags
		self.lastDst = None
		self.scopes = []
		self.currentScope = None
		self.lastOp = None
		
	def __str__(self):
		pieces = []
		for reg in self.regs:
			pieces.append(str(self.regs[reg]))
		for name in self.subroutines:
			pieces.append('\n'+str(self.subroutines[name]))
		for instruction in self.instructions:
			pieces.append('\n'+str(instruction))
		return ''.join(pieces)
		
	def writeHeader(self, otype, header):
		hFile = open(header, 'w')
		macro = header.upper().replace('.', '_')
		hFile.write('#ifndef {0}_'.format(macro))
		hFile.write('\n#define {0}_'.format(macro))
		hFile.write('\n#include "backend.h"')
		hFile.write('\n\ntypedef struct {')
		hFile.write('\n\tcpu_options gen;')
		hFile.write('\n}} {0}options;'.format(self.prefix))
		hFile.write('\n\ntypedef struct {')
		hFile.write('\n\t{0}options *opts;'.format(self.prefix))
		hFile.write('\n\tuint32_t cycles;')
		self.regs.writeHeader(otype, hFile)
		hFile.write('\n}} {0}context;'.format(self.prefix))
		hFile.write('\n')
		hFile.write('\n#endif //{0}_'.format(macro))
		hFile.write('\n')
		hFile.close()
	def build(self, otype):
		body = []
		pieces = []
		for include in self.includes:
			body.append('#include "{0}"\n'.format(include))
		for table in self.instructions:
			opmap = [None] * (1 << self.opsize)
			bodymap = {}
			instructions = self.instructions[table]
			instructions.sort()
			for inst in instructions:
				for val in inst.allValues():
					if opmap[val] is None:
						self.meta = {}
						self.temp = {}
						self.needFlagCoalesce = False
						self.needFlagDisperse = False
						self.lastOp = None
						opmap[val] = inst.generateName(val)
						bodymap[val] = inst.generateBody(val, self, otype)
			
			pieces.append('\ntypedef void (*impl_fun)({pre}context *context);'.format(pre=self.prefix))
			pieces.append('\nstatic impl_fun impl_{name}[{sz}] = {{'.format(name = table, sz=len(opmap)))
			for inst in range(0, len(opmap)):
				op = opmap[inst]
				if op is None:
					pieces.append('\n\tunimplemented,')
				else:
					pieces.append('\n\t' + op + ',')
					body.append(bodymap[inst])
			pieces.append('\n};')
		if self.body in self.subroutines:
			pieces.append('\nvoid {pre}execute({type} *context, uint32_t target_cycle)'.format(pre = self.prefix, type = self.context_type))
			pieces.append('\n{')
			pieces.append('\n\twhile (context->cycles < target_cycle)')
			pieces.append('\n\t{')
			self.meta = {}
			self.temp = {}
			self.subroutines[self.body].inline(self, [], pieces, otype, None)
			pieces.append('\n\t}')
			pieces.append('\n}')
		body.append('\nstatic void unimplemented({pre}context *context)'.format(pre = self.prefix))
		body.append('\n{')
		body.append('\n\tfatal_error("Unimplemented instruction");')
		body.append('\n}\n')
		return ''.join(body) +  ''.join(pieces)
		
	def checkBool(self, name):
		if not name in self.booleans:
			raise Exception(name + ' is not a defined boolean flag')
		return self.booleans[name]
	
	def getTemp(self, size):
		if size in self.temp:
			return ('', self.temp[size])
		self.temp[size] = 'tmp{sz}'.format(sz=size);
		return ('\n\tuint{sz}_t tmp{sz};'.format(sz=size), self.temp[size])
		
	def resolveParam(self, param, parent, fieldVals, allowConstant=True, isdst=False):
		keepGoing = True
		while keepGoing:
			keepGoing = False
			try:
				if type(param) is int:
					pass
				elif param.startswith('0x'):
					param = int(param, 16)
				else:
					param = int(param)
			except ValueError:
				
				if parent:
					if param in parent.regValues and allowConstant:
						return parent.regValues[param]
					maybeLocal = parent.resolveLocal(param)
					if maybeLocal:
						return maybeLocal
				if param in fieldVals:
					param = fieldVals[param]
				elif param in self.meta:
					param = self.meta[param]
					keepGoing = True
				elif self.isReg(param):
					param = self.resolveReg(param, parent, fieldVals, isdst)
		return param
	
	def isReg(self, name):
		if not type(name) is str:
			return False
		begin,sep,_ = name.partition('.')
		if sep:
			if begin in self.meta:
				begin = self.meta[begin]
			return self.regs.isRegArray(begin)
		else:
			return self.regs.isReg(name)
	
	def resolveReg(self, name, parent, fieldVals, isDst=False):
		begin,sep,end = name.partition('.')
		if sep:
			if begin in self.meta:
				begin = self.meta[begin]
			if not self.regs.isRegArrayMember(end):
				end = self.resolveParam(end, parent, fieldVals)
			if not type(end) is int and self.regs.isRegArrayMember(end):
				arrayName = self.regs.arrayMemberParent(end)
				end = self.regs.arrayMemberIndex(end)
				if arrayName != begin:
					end = 'context->{0}[{1}]'.format(arrayName, end)
			if self.regs.isNamedArray(begin):
				regName = self.regs.arrayMemberName(begin, end)
			else:
				regName = '{0}.{1}'.format(begin, end)
			ret = 'context->{0}[{1}]'.format(begin, end)
		else:
			regName = name
			if self.regs.isRegArrayMember(name):
				arr,idx = self.regs.regToArray[name]
				ret = 'context->{0}[{1}]'.format(arr, idx)
			else:
				ret = 'context->' + name
		if regName == self.flags.flagReg:
			if isDst:
				self.needFlagDisperse = True
			else:
				self.needFlagCoalesce = True
		if isDst:
			self.lastDst = regName
		return ret
		
	
	
	def paramSize(self, name):
		size = self.currentScope.localSize(name)
		if size:
			return size
		begin,sep,_ = name.partition('.')
		if sep and self.regs.isRegArray(begin):
			return self.regs.regArrays[begin][0]
		if self.regs.isReg(name):
			return self.regs.regs[name]
		return 32
	
	def pushScope(self, scope):
		self.scopes.append(scope)
		self.currentScope = scope
		
	def popScope(self):
		ret = self.scopes.pop()
		self.currentScope = self.scopes[-1] if self.scopes else None
		return ret
		
	def getRootScope(self):
		return self.scopes[0]

def parse(f):
	instructions = {}
	subroutines = {}
	registers = None
	flags = None
	errors = []
	info = {}
	line_num = 0
	cur_object = None
	for line in f:
		line_num += 1
		line,_,comment = line.partition('#')
		if not line.strip():
			continue
		if line[0].isspace():
			if not cur_object is None:
				parts = [el.strip() for el in line.split(' ')]
				if type(cur_object) is dict:
					cur_object[parts[0]] = parts[1:]
				else:
					cur_object = cur_object.processLine(parts)
				
#				if type(cur_object) is Registers:
#					if len(parts) > 2:
#						cur_object.addRegArray(parts[0], int(parts[1]), parts[2:])
#					else:
#						cur_object.addReg(parts[0], int(parts[1]))
#				elif type(cur_object) is dict:
#					cur_object[parts[0]] = parts[1:]
#				elif parts[0] == 'switch':
#					o = Switch(cur_object, parts[1])
#					cur_object.addOp(o)
#					cur_object = o
#				elif parts[0] == 'if':
#					o = If(cur_object, parts[1])
#					cur_object.addOp(o)
#					cur_object = o
#				elif parts[0] == 'end':
#					cur_object = cur_object.parent
#				else:
#					cur_object.addOp(NormalOp(parts))
			else:
				errors.append("Orphan instruction on line {0}".format(line_num))
		else:
			parts = line.split(' ')
			if len(parts) > 1:
				if len(parts) > 2:
					table,bitpattern,name = parts
				else:
					bitpattern,name = parts
					table = 'main'
				value = 0
				fields = {}
				curbit = len(bitpattern) - 1
				for char in bitpattern:
					value <<= 1
					if char in ('0', '1'):
						value |= int(char)
					else:
						if char in fields:
							fields[char] = (curbit, fields[char][1] + 1)
						else:
							fields[char] = (curbit, 1)
					curbit -= 1
				cur_object = Instruction(value, fields, name.strip())
				instructions.setdefault(table, []).append(cur_object)
			elif line.strip() == 'regs':
				if registers is None:
					registers = Registers()
				cur_object = registers
			elif line.strip() == 'info':
				cur_object = info
			elif line.strip() == 'flags':
				if flags is None:
					flags = Flags()
				cur_object = flags
			else:
				cur_object = SubRoutine(line.strip())
				subroutines[cur_object.name] = cur_object
	if errors:
		print(errors)
	else:
		p = Program(registers, instructions, subroutines, info, flags)
		p.booleans['dynarec'] = False
		p.booleans['interp'] = True
		
		if 'header' in info:
			print('#include "{0}"'.format(info['header'][0]))
			p.writeHeader('c', info['header'][0])
		print('#include "util.h"')
		print('#include <stdlib.h>')
		print(p.build('c'))

def main(argv):
	f =  open(argv[1])
	parse(f)

if __name__ == '__main__':
	from sys import argv
	main(argv)