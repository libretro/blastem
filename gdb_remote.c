#include "blastem.h"
#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#define INITIAL_BUFFER_SIZE 4096

char * buf = NULL;
char * curbuf = NULL;
size_t bufsize;
int cont = 0;
int expect_break_response=0;
uint32_t resume_pc;

void gdb_debug_enter(genesis_context * gen, uint32_t pc)
{
	resume_pc = pc;
	while(!cont)
	{
	}
	cont = 0;
}

void gdb_run_command(genesis_context * gen, char * command)
{
	switch(*command)
	{
	case 'c':
		if (*(command+1) != 0) {
			resume_pc =
		}
		cont = 1;
		expect_break_response = 1;
		break;
	case 's':

	}
}

void gdb_run_commands(genesis_context * gen)
{
	int enter_debugger = 0;
	char * cur = buf;
	while(cur < curbuf);
	{
		if(*cur == '$') {
			cur++
			char * start = cur;
			while (cur < curbuf && *cur != '#') {
				cur++;
			}
			if (*cur == '#') {
				//check to make sure we've received the checksum bytes
				if (curbuf-cur >= 2) {
					//TODO: verify checksum
					//Null terminate payload
					//send acknowledgement
					write(FILENO_STDOUT, "+", 1);
					gdb_run_command(genesis_context * gen, start);
					cur += 2;
				} else {
					cur = start - 1;
					break;
				}
			} else {
				cur = start - 1;
				break;
			}
		} else {
			if (*cur == 0x03) {
				enter_debugger = 1;
			}
			cur++;
		}
	}

	//FIXME
	if (consumed == curbuf-buf) {
		curbuf = buf;
	} else if (consumed > 0) {
		memmove(buf, buf + consumed, curbuf - buf - consumed);
		curbuf -= consumed;
	}
}

void gdb_command_poll(genesis_context * gen)
{
	for(;;)
	{
		if (curbuf == buf + bufsize) {
			//buffer is full, expand it
			bufsize *= 2;
			buf = realloc(buf, bufsize);
			if (!buf) {
				fprintf(stderr, "Failed to grow GDB command buffer to %d bytes\n", (int)bufsize);
				exit(1);
			}
			curbuf = buf + bufsize/2;
		}
		int numread = read(STDIN_FILENO, buf, bufsize - (curbuf-buf));
		if (numread < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return;
			} else {
				fprintf(stderr, "Error %d while reading GDB commands from stdin", errno);
				exit(1);
			}
		} else if (numread == 0) {
			exit(0);
		}
		gdb_run_commands(genesis_context * gen);
	}
}

void gdb_remote_init()
{
	fcntl(STDIN_FILENO, FD_SETFL, O_NONBLOCK);
	buf = malloc(INITIAL_BUFFER_SIZE);
	curbuf = buf;
	bufzie = INITIAL_BUFFER_SIZE;
}
