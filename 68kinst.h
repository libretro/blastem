#include <stdint.h>

typedef enum {
	BIT_MOVEP_IMMED = 0,
	MOVE_BYTE,
	MOVE_LONG,
	MOVE_WORD,
	MISC,
	QUICK_ARITH_LOOP,
	BRANCH,
	MOVEQ,
	OR_DIV_SBCD,
	SUB_SUBX,
	RESERVED,
	CMP_XOR,
	AND_MUL_ABCD_EXG,
	ADD_ADDX,
	SHIFT_ROTATE,
	COPROC
} m68k_optypes;

typedef enum {
	M68K_ABCD,
	M68K_ADD,
	M68K_ADDX,
	M68K_AND,
	M68K_ANDI_CCR,
	M68K_ANDI_SR,
	M68K_ASL,
	M68K_ASR,
	M68K_BCC,
	M68K_BCHG,
	M68K_BCLR,
	M68K_BSET,
	M68K_BSR,
	M68K_BTST,
	M68K_CHK,
	M68K_CLR,
	M68K_CMP,
	M68K_DBCC,
	M68K_DIVS,
	M68K_DIVU,
	M68K_EOR,
	M68K_EORI_CCR,
	M68K_EORI_SR,
	M68K_EXG,
	M68K_EXT,
	M68K_ILLEGAL,
	M68K_JMP,
	M68K_JSR,
	M68K_LEA,
	M68K_LINK,
	M68K_LSL,
	M68K_LSR,
	M68K_MOVE,
	M68K_MOVE_CCR,
	M68K_MOVE_FROM_SR,
	M68K_MOVE_SR,
	M68K_MOVE_USP,
	M68K_MOVEM,
	M68K_MOVEP,
	M68K_MULS,
	M68K_MULU,
	M68K_NBCD,
	M68K_NEG,
	M68K_NEGX,
	M68K_NOP,
	M68K_NOT,
	M68K_OR,
	M68K_ORI_CCR,
	M68K_ORI_SR,
	M68K_PEA,
	M68K_RESET,
	M68K_ROL,
	M68K_ROR,
	M68K_ROXL,
	M68K_ROXR,
	M68K_RTE,
	M68K_RTR,
	M68K_RTS,
	M68K_SBCD,
	M68K_SCC,
	M68K_STOP,
	M68K_SUB,
	M68K_SUBX,
	M68K_SWAP,
	M68K_TAS,
	M68K_TRAP,
	M68K_TRAPV,
	M68K_TST,
	M68K_UNLNK,
	M68K_INVALID
} m68K_op;

typedef enum {
	VAR_NORMAL,
	VAR_QUICK,
	VAR_BYTE,
	VAR_WORD,
	VAR_LONG
} m68K_variant;

typedef enum {
	OPSIZE_BYTE=0,
	OPSIZE_WORD,
	OPSIZE_LONG,
	OPSIZE_INVALID
} m68K_opsizes;

typedef enum {
//actual addressing mode field values
	MODE_REG = 0,
	MODE_AREG,
	MODE_AREG_INDIRECT,
	MODE_AREG_POSTINC,
	MODE_AREG_PREDEC,
	MODE_AREG_DISPLACE,
	MODE_AREG_INDEX_MEM, //bunch of relatively complicated modes
	MODE_PC_INDIRECT_ABS_IMMED, //Modes that use the program counter, an absolute address or immediate value
//expanded values
	MODE_ABSOLUTE_SHORT,
	MODE_ABSOLUTE,
	MODE_PC_DISPLACE,
	MODE_PC_INDEX,
	MODE_IMMEDIATE,
	MODE_UNUSED
} m68k_addr_modes;

typedef enum {
	COND_TRUE,
	COND_FALSE,
	COND_HIGH,
	COND_LOW_SAME,
	COND_CARRY_CLR,
	COND_CARRY_SET,
	COND_NOT_EQ,
	COND_EQ,
	COND_OVERF_CLR,
	COND_OVERF_SET,
	COND_PLUS,
	COND_MINUS,
	COND_GREATER_EQ,
	COND_LESS,
	COND_GREATER,
	COND_LESS_EQ
} m68K_condition;

typedef struct {
	uint8_t addr_mode;
	union {
		struct {
			uint8_t pri;
			uint8_t sec;
			int32_t displacement;
		} regs;
		uint8_t u8;
		uint16_t u16;
		uint32_t u32;
	} params;
} m68k_op_info;

typedef struct {
	uint8_t op;
	uint8_t variant;
	union {
		uint8_t size;
		uint8_t cond;
	} extra;
	m68k_op_info src;
	m68k_op_info dst;
} m68kinst;

uint16_t * m68K_decode(uint16_t * istream, m68kinst * dst);
uint32_t m68k_cycles(m68kinst * inst);
int m68K_disasm(m68kinst * decoded, char * dst);

