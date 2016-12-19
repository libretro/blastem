#include <stdint.h>
#include <stdio.h>
#include "jagcpu.h"

char *gpu_mnemonics[] = {
	"add",
	"addc",
	"addq",
	"addqt",
	"sub",
	"subc",
	"subq",
	"subqt",
	"neg",
	"and",
	"or",
	"xor",
	"not",
	"btst",
	"bset",
	"bclr",
	"mult",
	"imult",
	"imultn",
	"resmac",
	"imacn",
	"div",
	"abs",
	"sh",
	"shlq",
	"shrq",
	"sha",
	"sharq",
	"ror",
	"rorq",
	"cmp",
	"cmpq",
	"sat8",
	"sat16",
	"move",
	"moveq",
	"moveta",
	"movefa",
	"movei",
	"loadb",
	"loadw",
	"load",
	"loadp",
	"load", //r14 relative
	"load", //r15 relative
	"storeb",
	"storew",
	"store",
	"storep",
	"store", //r14 relative
	"store", //r15 relative
	"move", //PC
	"jump",
	"jr",
	"mmult",
	"mtoi",
	"normi",
	"nop",
	"load", //r14 indexed
	"load", //r15 indexed
	"store", //r14 indexed
	"store", //r15 indexed
	"sat24",
	"pack",
	"unpack"
};

char *dsp_mnemonics[DSP_ADDQMOD+1] = {
	[DSP_SUBQMOD] = "subqmod",
	[DSP_SAT16S] = "sat16s",
	[DSP_SAT32S] = "sat32s",
	[DSP_MIRROR] = "mirror",
	[DSP_ADDQMOD] = "addqmod"
};

void init_dsp_mnemonic_table()
{
	static uint8_t init_done;
	if (init_done) {
		return;
	}
	for (int i = 0; i < DSP_ADDQMOD; i++)
	{
		if (!dsp_mnemonics[i]) {
			dsp_mnemonics[i] = gpu_mnemonics[i];
		}
	}
	init_done = 1;
}

uint16_t jag_opcode(uint16_t inst, uint8_t is_gpu)
{
	uint16_t opcode = inst >> 10;
	if (is_gpu && opcode == GPU_PACK && (inst & 0x20)) {
		return GPU_UNPACK;
	}
	return opcode;
}

uint16_t jag_reg2(uint16_t inst)
{
	return inst & 0x1F;
}

uint16_t jag_reg1(uint16_t inst)
{
	return inst >> 5 & 0x1F;
}

//moveq and bit instructions should just use jag_reg1 instead
uint32_t jag_quick(uint16_t inst)
{
	uint32_t val = inst >> 5 & 0x1F;
	return val ? val : 32;
}

uint8_t is_quick_1_32_opcode(uint16_t opcode, uint8_t is_gpu)
{
	return opcode == JAG_ADDQ
		|| opcode == JAG_ADDQT
		|| opcode == JAG_SUBQ
		|| opcode == JAG_SUBQT
		|| opcode == JAG_SHLQ
		|| opcode == JAG_SHRQ
		|| opcode == JAG_SHARQ
		|| opcode == JAG_RORQ
		|| (!is_gpu && (
			opcode == DSP_SUBQMOD
			|| opcode == DSP_ADDQMOD
		));
}

uint8_t is_quick_0_31_opcode(uint16_t opcode)
{
	return opcode == JAG_MOVEQ
		|| opcode == JAG_BTST
		|| opcode == JAG_BSET
		|| opcode == JAG_BCLR;
}

uint8_t is_load(uint16_t opcode, uint8_t is_gpu)
{
	return opcode == JAG_LOAD
		|| opcode == JAG_LOADB
		|| opcode == JAG_LOADW
		|| (is_gpu && opcode == GPU_LOADP);
}

uint8_t is_store(uint16_t opcode, uint8_t is_gpu)
{
	return opcode == JAG_STORE
		|| opcode == JAG_STOREB
		|| opcode == JAG_STOREW
		|| (is_gpu && opcode == GPU_STOREP);
}

uint8_t is_single_source(uint16_t opcode, uint8_t is_gpu)
{
	return opcode == JAG_NEG
		|| opcode == JAG_NOT
		|| opcode == JAG_ABS
		|| opcode == GPU_SAT16 //no is_gpu check needed as DSP_SAT16S is also single source
		|| (is_gpu && (
			opcode == GPU_SAT8
			|| opcode == GPU_SAT24
			|| opcode == GPU_PACK
			|| opcode == GPU_UNPACK
		))
		|| (!is_gpu && (
			opcode == DSP_SAT32S
			|| opcode == DSP_MIRROR
		));
}

char * jag_cc_names[] = {
	"t",
	"ne",
	"eq",
	"f",
	"cc",
	"hi",
	"eq_cc",
	"f",
	"cs",
	"ne_cs",
	"eq_cs",
	"f",
	"f",
	"f",
	"f",
	"f"
	"t_alt",
	"ne_alt",
	"eq_alt",
	"f",
	"pl",
	"ne_pl",
	"eq_pl",
	"f",
	"mi",
	"ne_mi",
	"eq_mi"
};

char * jag_cc(uint16_t inst)
{
	uint16_t ccnum = jag_reg2(inst);
	if (ccnum >= sizeof(jag_cc_names)/sizeof(*jag_cc_names)) {
		return jag_cc_names[3];
	}
	return jag_cc_names[ccnum];
}

uint8_t jag_is_alwyas_falsse(uint16_t cond)
{
	return (cond & 3) == 3 || (cond && 0xC) == 0xC;
}

uint32_t jag_jr_dest(uint16_t inst, uint32_t address)
{
	uint32_t rel = jag_reg1(inst);
	if (rel & 0x10) {
		rel |= 0xFFFFFFE0;
	}
	return address + 2 + rel*2;
}

int jag_cpu_disasm(uint16_t **stream, uint32_t address, char *dst, uint8_t is_gpu, uint8_t labels)
{
	uint16_t inst = **stream;
	(*stream)++;
	uint16_t opcode = jag_opcode(inst, is_gpu);
	char **mnemonics;
	if (is_gpu) {
		mnemonics =  gpu_mnemonics;
	} else {
		init_dsp_mnemonic_table();
		mnemonics = dsp_mnemonics;
	}
	switch (opcode)
	{
	case JAG_MOVEI: {
		uint32_t immed = **stream;
		(*stream)++;
		immed |= **stream << 16;
		(*stream)++;
		return sprintf(dst, "%s $%X, r%d", mnemonics[opcode], immed, jag_reg2(inst));
	}
	case JAG_JR:
		return sprintf(dst,
			labels ? "%s %s, ADR_%X" : "%s %s, $W%X",
			mnemonics[opcode], jag_cc(inst), jag_jr_dest(inst, address)
		);
	case JAG_JUMP:
		return sprintf(dst, "%s %s, (r%d)", mnemonics[opcode], jag_cc(inst), jag_reg1(inst));
	case JAG_LOAD_R14_REL:
	case JAG_LOAD_R15_REL:
		return sprintf(dst, "%s (r%d+%d), r%d",  
			mnemonics[opcode],
			opcode == JAG_LOAD_R14_REL ? 14 : 15,
			jag_quick(inst),
			jag_reg2(inst)
		);
		break;
	case JAG_STORE_R14_REL:
	case JAG_STORE_R15_REL:
		return sprintf(dst, "%s r%d, (r%d+%d)",  
			mnemonics[opcode],
			jag_reg2(inst),
			opcode == JAG_STORE_R14_REL ? 14 : 15,
			jag_quick(inst)
		);
		break;
	case JAG_LOAD_R14_INDEXED:
	case JAG_LOAD_R15_INDEXED:
		return sprintf(dst, "%s (r%d+r%d), r%d",  
			mnemonics[opcode],
			opcode == JAG_LOAD_R14_INDEXED ? 14 : 15,
			jag_reg1(inst),
			jag_reg2(inst)
		);
		break;
	case JAG_STORE_R14_INDEXED:
	case JAG_STORE_R15_INDEXED:
		return sprintf(dst, "%s r%d, (r%d+r%d)",  
			mnemonics[opcode],
			jag_reg2(inst),
			opcode == JAG_STORE_R14_INDEXED ? 14 : 15,
			jag_reg1(inst)
		);
		break;
	default:
		if (is_quick_1_32_opcode(opcode, is_gpu)) {
			return sprintf(dst, "%s %d, r%d", mnemonics[opcode], jag_quick(inst), jag_reg2(inst));
		} else if (is_quick_0_31_opcode(opcode)) {
			return sprintf(dst, "%s %d, r%d", mnemonics[opcode], jag_reg1(inst), jag_reg2(inst));
		} else if (is_load(opcode, is_gpu)) {
			return sprintf(dst, "%s (r%d), r%d", mnemonics[opcode], jag_reg1(inst), jag_reg2(inst));
		} else if (is_store(opcode, is_gpu)) {
			return sprintf(dst, "%s r%d, (r%d)", mnemonics[opcode], jag_reg2(inst), jag_reg1(inst));
		} else {
			return sprintf(dst, "%s r%d, r%d", mnemonics[opcode], jag_reg1(inst), jag_reg2(inst));
		}
	}
}
