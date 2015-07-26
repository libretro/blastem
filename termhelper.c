#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <stdlib.h>
#include "terminal.h"

char buf[4096];

void copy_data(int to, int from)
{
	ssize_t bytes = read(from, buf, sizeof(buf));
	while (bytes > 0)
	{
		ssize_t written = write(to, buf, bytes);
		if (written == -1) {
			exit(1);
		}
		bytes -= written;
	}
}

int main(int argc, char **argv)
{
	//these will block so order is important
	int input_fd = open(INPUT_PATH, O_WRONLY);
	int output_fd = open(OUTPUT_PATH, O_RDONLY);
	fd_set read_fds;
	FD_ZERO(&read_fds);
	for (;;)
	{
		FD_SET(STDIN_FILENO, &read_fds);
		FD_SET(output_fd, &read_fds);
		select(output_fd+1, &read_fds, NULL, NULL, NULL);
		
		if (FD_ISSET(STDIN_FILENO, &read_fds)) {
			copy_data(input_fd, STDIN_FILENO);
		}
		if (FD_ISSET(output_fd, &read_fds)) {
			copy_data(STDOUT_FILENO, output_fd);
		}
	}
	return 0;
}
