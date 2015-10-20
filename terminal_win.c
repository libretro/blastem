#include <windows.h>
#include <stdio.h>

static char init_done;

void force_no_terminal()
{
	init_done = 1;
}

void init_terminal()
{
	if (!init_done) {
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
		init_done = 1;
	}
}
