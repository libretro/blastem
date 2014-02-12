/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "gdb_remote.h"
#include "68kinst.h"
#include "debug.h"
#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#define INITIAL_BUFFER_SIZE (16*1024)

#ifdef DO_DEBUG_PRINT
#define dfprintf fprintf
#else
#define dfprintf
#endif

char * buf = NULL;
char * curbuf = NULL;
char * end = NULL;
size_t bufsize;
int cont = 0;
int expect_break_response=0;
uint32_t resume_pc;


static uint16_t branch_t;
static uint16_t branch_f;

static bp_def * breakpoints = NULL;
static uint32_t bp_index = 0;


void hex_32(uint32_t num, char * out)
{
	for (int32_t shift = 28; shift >= 0; shift -= 4)
	{
		uint8_t nibble = num >> shift & 0xF;
		*(out++) = nibble > 9 ? nibble - 0xA + 'A' : nibble + '0';
	}
}

void hex_16(uint16_t num, char * out)
{
	for (int16_t shift = 14; shift >= 0; shift -= 4)
	{
		uint8_t nibble = num >> shift & 0xF;
		*(out++) = nibble > 9 ? nibble - 0xA + 'A' : nibble + '0';
	}
}

void hex_8(uint8_t num, char * out)
{
	uint8_t nibble = num >> 4;
	*(out++) = nibble > 9 ? nibble - 0xA + 'A' : nibble + '0';
	nibble = num & 0xF;
	*out = nibble > 9 ? nibble - 0xA + 'A' : nibble + '0';
}

void gdb_calc_checksum(char * command, char *out)
{
	uint8_t checksum = 0;
	while (*command)
	{
		checksum += *(command++);
	}
	hex_8(checksum, out);
}

void write_or_die(int fd, const void *buf, size_t count)
{
	if (write(fd, buf, count) < count) {
		fputs("Error writing to stdout\n", stderr);
		exit(1);
	}
}

void gdb_send_command(char * command)
{
	char end[3];
	write_or_die(STDOUT_FILENO, "$", 1);
	write_or_die(STDOUT_FILENO, command, strlen(command));
	end[0] = '#';
	gdb_calc_checksum(command, end+1);
	write_or_die(STDOUT_FILENO, end, 3);
	dfprintf(stderr, "Sent $%s#%c%c\n", command, end[1], end[2]);
}

uint32_t calc_status(m68k_context * context)
{
	uint32_t status = context->status << 3;
	for (int i = 0; i < 5; i++)
	{
		status <<= 1;
		status |= context->flags[i];
	}
	return status;
}

uint8_t read_byte(m68k_context * context, uint32_t address)
{
	uint16_t * word;
	//TODO: Use generated read/write functions so that memory map is properly respected
	if (address < 0x400000) {
		word = context->mem_pointers[0] + address/2;
	} else if (address >= 0xE00000) {
		word = context->mem_pointers[1] + (address & 0xFFFF)/2;
	} else {
		return 0;
	}
	if (address & 1) {
		return *word;
	}
	return *word >> 8;
}

void gdb_run_command(m68k_context * context, uint32_t pc, char * command)
{
	char send_buf[512];
	dfprintf(stderr, "Received command %s\n", command);
	switch(*command)
	{

	case 'c':
		if (*(command+1) != 0) {
			//TODO: implement resuming at an arbitrary address
			goto not_impl;
		}
		cont = 1;
		expect_break_response = 1;
		break;
	case 's': {
		if (*(command+1) != 0) {
			//TODO: implement resuming at an arbitrary address
			goto not_impl;
		}
		m68kinst inst;
		uint16_t * pc_ptr;
		if (pc < 0x400000) {
			pc_ptr = cart + pc/2;
		} else if(pc > 0xE00000) {
			pc_ptr = ram + (pc & 0xFFFF)/2;
		} else {
			fprintf(stderr, "Entered gdb remote debugger stub at address %X\n", pc);
			exit(1);
		}
		uint16_t * after_pc = m68k_decode(pc_ptr, &inst, pc & 0xFFFFFF);
		uint32_t after = pc + (after_pc-pc_ptr)*2;

		if (inst.op == M68K_RTS) {
			after = (read_dma_value(context->aregs[7]/2) << 16) | read_dma_value(context->aregs[7]/2 + 1);
		} else if (inst.op == M68K_RTE || inst.op == M68K_RTR) {
			after = (read_dma_value((context->aregs[7]+2)/2) << 16) | read_dma_value((context->aregs[7]+2)/2 + 1);
		} else if(m68k_is_branch(&inst)) {
			if (inst.op == M68K_BCC && inst.extra.cond != COND_TRUE) {
				branch_f = after;
				branch_t = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
				insert_breakpoint(context, branch_t, (uint8_t *)gdb_debug_enter);
			} else if(inst.op == M68K_DBCC && inst.extra.cond != COND_FALSE) {
				branch_t = after;
				branch_f = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
				insert_breakpoint(context, branch_f, (uint8_t *)gdb_debug_enter);
			} else {
				after = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
			}
		}
		insert_breakpoint(context, after, (uint8_t *)gdb_debug_enter);

		cont = 1;
		expect_break_response = 1;
		break;
	}
	case 'H':
		if (command[1] == 'g' || command[1] == 'c') {;
			//no thread suport, just acknowledge
			gdb_send_command("OK");
		} else {
			goto not_impl;
		}
		break;
	case 'Z': {
		uint8_t type = command[1];
		if (type < '2') {
			uint32_t address = strtoul(command+3, NULL, 16);
			insert_breakpoint(context, address, (uint8_t *)gdb_debug_enter);
			bp_def *new_bp = malloc(sizeof(bp_def));
			new_bp->next = breakpoints;
			new_bp->address = address;
			new_bp->index = bp_index++;
			breakpoints = new_bp;
			gdb_send_command("OK");
		} else {
			//watchpoints are not currently supported
			gdb_send_command("");
		}
		break;
	}
	case 'z': {
		uint8_t type = command[1];
		if (type < '2') {
			uint32_t address = strtoul(command+3, NULL, 16);
			remove_breakpoint(context, address);
			bp_def **found = find_breakpoint(&breakpoints, address);
			if (*found)
			{
				bp_def * to_remove = *found;
				*found = to_remove->next;
				free(to_remove);
			}
			gdb_send_command("OK");
		} else {
			//watchpoints are not currently supported
			gdb_send_command("");
		}
		break;
	}
	case 'g': {
		char * cur = send_buf;
		for (int i = 0; i < 8; i++)
		{
			hex_32(context->dregs[i], cur);
			cur += 8;
		}
		for (int i = 0; i < 8; i++)
		{
			hex_32(context->aregs[i], cur);
			cur += 8;
		}
		hex_32(calc_status(context), cur);
		cur += 8;
		hex_32(pc, cur);
		cur += 8;
		*cur = 0;
		gdb_send_command(send_buf);
		break;
	}
	case 'm': {
		char * rest;
		uint32_t address = strtoul(command+1, &rest, 16);
		uint32_t size = strtoul(rest+1, NULL, 16);
		if (size > sizeof(send_buf-1)/2) {
			size = sizeof(send_buf-1)/2;
		}
		char *cur = send_buf;
		while (size)
		{
			hex_8(read_byte(context, address), cur);
			cur += 2;
			address++;
			size--;
		}
		*cur = 0;
		gdb_send_command(send_buf);
		break;
	}
	case 'p': {
		unsigned long reg = strtoul(command+1, NULL, 16);

		if (reg < 8) {
			hex_32(context->dregs[reg], send_buf);
		} else if (reg < 16) {
			hex_32(context->aregs[reg-8], send_buf);
		} else if (reg == 16) {
			hex_32(calc_status(context), send_buf);
		} else if (reg == 17) {
			hex_32(pc, send_buf);
		} else {
			send_buf[0] = 0;
		}
		send_buf[8] = 0;
		gdb_send_command(send_buf);
		break;
	}
	case 'q':
		if (!memcmp("Supported", command+1, strlen("Supported"))) {
			sprintf(send_buf, "PacketSize=%X", (int)bufsize);
			gdb_send_command(send_buf);
		} else if (!memcmp("Attached", command+1, strlen("Supported"))) {
			//not really meaningful for us, but saying we spawned a new process
			//is probably closest to the truth
			gdb_send_command("0");
		} else if (!memcmp("Offsets", command+1, strlen("Offsets"))) {
			//no relocations, so offsets are all 0
			gdb_send_command("Text=0;Data=0;Bss=0");
		} else if (!memcmp("Symbol", command+1, strlen("Symbol"))) {
			gdb_send_command("");
		} else if (!memcmp("TStatus", command+1, strlen("TStatus"))) {
			//TODO: actual tracepoint support
			gdb_send_command("T0;tnotrun:0");
		} else if (!memcmp("TfV", command+1, strlen("TfV")) || !memcmp("TfP", command+1, strlen("TfP"))) {
			//TODO: actual tracepoint support
			gdb_send_command("");
		} else if (command[1] == 'C') {
			//we only support a single thread currently, so send 1
			gdb_send_command("1");
		} else {
			goto not_impl;
		}
		break;
	case 'v':
		if (!memcmp("Cont?", command+1, strlen("Cont?"))) {
			gdb_send_command("vCont;c;C;s;S");
		} else if (!memcmp("Cont;", command+1, strlen("Cont;"))) {
			switch (*(command + 1 + strlen("Cont;")))
			{
			case 'c':
			case 'C':
				//might be interesting to have continue with signal fire a
				//trap exception or something, but for no we'll treat it as
				//a normal continue
				cont = 1;
				expect_break_response = 1;
				break;
			case 's':
			case 'S': {
				m68kinst inst;
				uint16_t * pc_ptr;
				if (pc < 0x400000) {
					pc_ptr = cart + pc/2;
				} else if(pc > 0xE00000) {
					pc_ptr = ram + (pc & 0xFFFF)/2;
				} else {
					fprintf(stderr, "Entered gdb remote debugger stub at address %X\n", pc);
					exit(1);
				}
				uint16_t * after_pc = m68k_decode(pc_ptr, &inst, pc & 0xFFFFFF);
				uint32_t after = pc + (after_pc-pc_ptr)*2;

				if (inst.op == M68K_RTS) {
					after = (read_dma_value(context->aregs[7]/2) << 16) | read_dma_value(context->aregs[7]/2 + 1);
				} else if (inst.op == M68K_RTE || inst.op == M68K_RTR) {
					after = (read_dma_value((context->aregs[7]+2)/2) << 16) | read_dma_value((context->aregs[7]+2)/2 + 1);
				} else if(m68k_is_branch(&inst)) {
					if (inst.op == M68K_BCC && inst.extra.cond != COND_TRUE) {
						branch_f = after;
						branch_t = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
						insert_breakpoint(context, branch_t, (uint8_t *)gdb_debug_enter);
					} else if(inst.op == M68K_DBCC && inst.extra.cond != COND_FALSE) {
						branch_t = after;
						branch_f = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
						insert_breakpoint(context, branch_f, (uint8_t *)gdb_debug_enter);
					} else {
						after = m68k_branch_target(&inst, context->dregs, context->aregs) & 0xFFFFFF;
					}
				}
				insert_breakpoint(context, after, (uint8_t *)gdb_debug_enter);

				cont = 1;
				expect_break_response = 1;
				break;
			}
			default:
				goto not_impl;
			}
		} else {
			goto not_impl;
		}
		break;
	case '?':
		gdb_send_command("S05");
		break;
	default:
		goto not_impl;

	}
	return;
not_impl:
	fprintf(stderr, "Command %s is not implemented, exiting...\n", command);
	exit(1);
}

m68k_context *  gdb_debug_enter(m68k_context * context, uint32_t pc)
{
	dfprintf(stderr, "Entered debugger at address %X\n", pc);
	if (expect_break_response) {
		gdb_send_command("S05");
		expect_break_response = 0;
	}
	if ((pc & 0xFFFFFF) == branch_t) {
		bp_def ** f_bp = find_breakpoint(&breakpoints, branch_f);
		if (!*f_bp) {
			remove_breakpoint(context, branch_f);
		}
		branch_t = branch_f = 0;
	} else if((pc & 0xFFFFFF) == branch_f) {
		bp_def ** t_bp = find_breakpoint(&breakpoints, branch_t);
		if (!*t_bp) {
			remove_breakpoint(context, branch_t);
		}
		branch_t = branch_f = 0;
	}
	//Check if this is a user set breakpoint, or just a temporary one
	bp_def ** this_bp = find_breakpoint(&breakpoints, pc & 0xFFFFFF);
	if (!*this_bp) {
		remove_breakpoint(context, pc & 0xFFFFFF);
	}
	resume_pc = pc;
	cont = 0;
	uint8_t partial = 0;
	while(!cont)
	{
		if (!curbuf) {
			int numread = read(STDIN_FILENO, buf, bufsize);
			curbuf = buf;
			end = buf + numread;
		} else if (partial) {
			if (curbuf != buf) {
				memmove(curbuf, buf, end-curbuf);
				end -= curbuf - buf;
			}
			int numread = read(STDIN_FILENO, end, bufsize - (end-buf));
			end += numread;
			curbuf = buf;
		}
		for (; curbuf < end; curbuf++)
		{
			if (*curbuf == '$')
			{
				curbuf++;
				char * start = curbuf;
				while (curbuf < end && *curbuf != '#') {
					curbuf++;
				}
				if (*curbuf == '#') {
					//check to make sure we've received the checksum bytes
					if (end-curbuf >= 2) {
						//TODO: verify checksum
						//Null terminate payload
						*curbuf = 0;
						//send acknowledgement
						if (write(STDOUT_FILENO, "+", 1) < 1) {
							fputs("Error writing to stdout\n", stderr);
							exit(1);
						}
						gdb_run_command(context, pc, start);
						curbuf += 2;
					}
				} else {
					curbuf--;
					partial = 1;
					break;
				}
			} else {
				dfprintf(stderr, "Ignoring character %c\n", *curbuf);
			}
		}
		if (curbuf == end) {
			curbuf = NULL;
		}
	}
	return context;
}

void gdb_remote_init(void)
{
	buf = malloc(INITIAL_BUFFER_SIZE);
	curbuf = NULL;
	bufsize = INITIAL_BUFFER_SIZE;
}
