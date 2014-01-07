/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "blastem.h"
#include <unistd.h>
#include <fcntl.h>
#include <stddef.h>
#include <stdlib.h>
#include <stdio.h>

#define INITIAL_BUFFER_SIZE 4096

char * buf = NULL;
char * curbuf = NULL;
char * end = NULL;
size_t bufsize;
int cont = 0;
int expect_break_response=0;
uint32_t resume_pc;

void gdb_debug_enter(genesis_context * gen, uint32_t pc)
{
	fcntl(STDIN_FILENO, FD_SETFL, 0);
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
				end -= cufbuf - buf;
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
						*curbuf = 0
						//send acknowledgement
						write(FILENO_STDOUT, "+", 1);
						gdb_run_command(genesis_context * gen, start);
						curbuf += 2;
					}
				} else {
					curbuf--;
					partial = 1;
					break;
				}
			}
		}
	}
	fcntl(STDIN_FILENO, FD_SETFL, O_NONBLOCK);
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

int gdb_command_poll(genesis_context * gen)
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
		int numread = read(STDIN_FILENO, buf, bufsize);
		if (numread < 0) {
			if (errno == EAGAIN || errno == EWOULDBLOCK) {
				return 0;
			} else {
				fprintf(stderr, "Error %d while reading GDB commands from stdin", errno);
				exit(1);
			}
		} else if (numread == 0) {
			exit(0);
		}
		for (curbuf = buf, end = buf+numread; curbuf < end; curbuf++)
		{
			if (*curbuf = 0x03)
			{
				curbuf++;
				return 1;
			}
		}
	}
	return 0;
}

void gdb_remote_init()
{
	fcntl(STDIN_FILENO, FD_SETFL, O_NONBLOCK);
	buf = malloc(INITIAL_BUFFER_SIZE);
	curbuf = buf;
	bufzie = INITIAL_BUFFER_SIZE;
}
