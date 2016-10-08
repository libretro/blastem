#include <stdint.h>
#include <stdlib.h>
#include "jag_video.h"

jag_video *jag_video_init(void)
{
	return calloc(1, sizeof(jag_video));
}

void jag_video_run(jag_video *context, uint32_t target_cycle)
{
	context->cycles = target_cycle;
}
