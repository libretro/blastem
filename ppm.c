#include <stdint.h>
#include <stdio.h>

void save_ppm(FILE *f, uint32_t *buffer, uint32_t width, uint32_t height, uint32_t pitch)
{
	fprintf(f, "P6\n%d %d\n255\n", width, height);
	for(uint32_t y = 0; y < height; y++)
	{
		uint32_t *line = buffer;
		for (uint32_t x = 0; x < width; x++, line++)
		{
			uint8_t buf[3] = {
				*line >> 16, //red
				*line >> 8,  //green
				*line        //blue
			};
			fwrite(buf, 1, sizeof(buf), f);
		}
		buffer = buffer + pitch / sizeof(uint32_t);
	}
}
