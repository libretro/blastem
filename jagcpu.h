#ifndef JAGCPU_H_
#define JAGCPU_H_

enum {
	JAG_ADD,
	JAG_ADDC,
	JAG_ADDQ,
	JAG_ADDQT,
	JAG_SUB,
	JAG_SUBC,
	JAG_SUBQ,
	JAG_SUBQT,
	JAG_NEG,
	JAG_AND,
	JAG_OR,
	JAG_XOR,
	JAG_NOT,
	JAG_BTST,
	JAG_BSET,
	JAG_BCLR,
	JAG_MULT,
	JAG_IMULT,
	JAG_IMULTN,
	JAG_RESMAC,
	JAG_IMACN,
	JAG_DIV,
	JAG_ABS,
	JAG_SH,
	JAG_SHLQ,
	JAG_SHRQ,
	JAG_SHA,
	JAG_SHARQ,
	JAG_ROR,
	JAG_RORQ,
	JAG_CMP,
	JAG_CMPQ,
	GPU_SAT8,
	DSP_SUBQMOD = GPU_SAT8,
	GPU_SAT16,
	DSP_SAT16S = GPU_SAT16,
	JAG_MOVE,
	JAG_MOVEQ,
	JAG_MOVETA,
	JAG_MOVEFA,
	JAG_MOVEI,
	JAG_LOADB,
	JAG_LOADW,
	JAG_LOAD,
	GPU_LOADP,
	DSP_SAT32S = GPU_LOADP,
	JAG_LOAD_R14_REL,
	JAG_LOAD_R15_REL,
	JAG_STOREB,
	JAG_STOREW,
	JAG_STORE,
	GPU_STOREP,
	DSP_MIRROR = GPU_STOREP,
	JAG_STORE_R14_REL,
	JAG_STORE_R15_REL,
	JAG_MOVE_PC,
	JAG_JUMP,
	JAG_JR,
	JAG_MMULT,
	JAG_MTOI,
	JAG_NORMI,
	JAG_NOP,
	JAG_LOAD_R14_INDEXED,
	JAG_LOAD_R15_INDEXED,
	JAG_STORE_R14_INDEXED,
	JAG_STORE_R15_INDEXED,
	GPU_SAT24,
	GPU_PACK,
	DSP_ADDQMOD = GPU_PACK,
	GPU_UNPACK //virtual opcode, UNPACK is PACK with a reg1 field set to 1
};

#define JAGCPU_NOREG -1


typedef struct {
	cpu_options gen;
	int8_t      regs[32];
	int8_t      result;
	int8_t      resultreg;
	int8_t      bankptr;
	uint8_t     is_gpu;
} jag_cpu_options;

typedef struct {
	uint32_t read_high;
} jag_gpu;

typedef struct {
	uint8_t  mac_high;
	uint8_t  modulo;
} jag_dsp;

typedef struct {
	uint32_t cycles;
	uint32_t regs[64];
	uint32_t *main;
	uint32_t *alt;
	uint32_t pc;
	uint32_t result;
	uint32_t flags;
	uint32_t flags_pending;
	uint32_t remainder;
	union {
		jag_gpu gpu;
		jag_dsp dsp;
	};
	int8_t   writeback;
	int8_t   resultreg;
	uint8_t  is_gpu;
} jag_cpu;

uint16_t jag_opcode(uint16_t inst, uint8_t is_gpu);
char * jag_cc(uint16_t inst);
uint16_t jag_reg2(uint16_t inst);
uint32_t jag_jr_dest(uint16_t inst, uint32_t address);
int jag_cpu_disasm(uint16_t **stream, uint32_t address, char *dst, uint8_t is_gpu, uint8_t labels);

#endif
