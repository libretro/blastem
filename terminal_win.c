#include <windows.h>
#include <stdio.h>

void init_terminal()
{
	static char init_done;
	if (!init_done) {
		AllocConsole();
		freopen("CONIN$", "r", stdin);
		freopen("CONOUT$", "w", stdout);
		freopen("CONOUT$", "w", stderr);
	}
}
