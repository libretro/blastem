#include <stdlib.h>
#include <string.h>
#include "libretro.h"
#include "system.h"
#include "util.h"
#include "vdp.h"
#include "render.h"
#include "io.h"

static retro_environment_t retro_environment;
RETRO_API void retro_set_environment(retro_environment_t re)
{
	retro_environment = re;
}

static retro_video_refresh_t retro_video_refresh;
RETRO_API void retro_set_video_refresh(retro_video_refresh_t rvf)
{
	retro_video_refresh = rvf;
}

static retro_audio_sample_t retro_audio_sample;
RETRO_API void retro_set_audio_sample(retro_audio_sample_t ras)
{
	retro_audio_sample = ras;
}

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t rasb)
{
}

static retro_input_poll_t retro_input_poll;
RETRO_API void retro_set_input_poll(retro_input_poll_t rip)
{
	retro_input_poll = rip;
}

static retro_input_state_t retro_input_state;
RETRO_API void retro_set_input_state(retro_input_state_t ris)
{
	retro_input_state = ris;
}

int headless = 0;
int exit_after = 0;
int z80_enabled = 1;
char *save_filename;
tern_node *config;
uint8_t use_native_states = 1;
system_header *current_system;
system_media media;

RETRO_API void retro_init(void)
{
}

RETRO_API void retro_deinit(void)
{
}

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	info->library_name = "BlastEm";
	info->library_version = "0.6.2-pre"; //TODO: share this with blastem.c
	info->valid_extensions = "md|gen|sms|bin|rom";
	info->need_fullpath = 0;
	info->block_extract = 0;
}

static vid_std video_standard;
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	info->geometry.base_width = info->geometry.max_width = LINEBUF_SIZE;
	info->geometry.base_height = info->geometry.max_height = video_standard == VID_NTSC ? 243 : 294;
	info->geometry.aspect_ratio = 0;
	info->timing.fps = video_standard == VID_NTSC ? 60 : 50;
	info->timing.sample_rate = 53267; //approximate sample rate of YM2612
}

RETRO_API void retro_set_controller_port_device(unsigned port, unsigned device)
{
}

/* Resets the current game. */
RETRO_API void retro_reset(void)
{
	current_system->soft_reset(current_system);
}

/* Runs the game for one video frame.
 * During retro_run(), input_poll callback must be called at least once.
 *
 * If a frame is not rendered for reasons where a game "dropped" a frame,
 * this still counts as a frame, and retro_run() should explicitly dupe
 * a frame if GET_CAN_DUPE returns true.
 * In this case, the video callback can take a NULL argument for data.
 */
static uint8_t started;
RETRO_API void retro_run(void)
{
	if (started) {
		current_system->resume_context(current_system);
	} else {
		current_system->start_context(current_system, NULL);
		started = 1;
	}
}

/* Returns the amount of data the implementation requires to serialize
 * internal state (save states).
 * Between calls to retro_load_game() and retro_unload_game(), the
 * returned size is never allowed to be larger than a previous returned
 * value, to ensure that the frontend can allocate a save state buffer once.
 */
static size_t serialize_size_cache;
RETRO_API size_t retro_serialize_size(void)
{
	if (!serialize_size_cache) {
		uint8_t *tmp = current_system->serialize(current_system, &serialize_size_cache);
		free(tmp);
	}
	return serialize_size_cache;
}

/* Serializes internal state. If failed, or size is lower than
 * retro_serialize_size(), it should return false, true otherwise. */
RETRO_API bool retro_serialize(void *data, size_t size)
{
	size_t actual_size;
	uint8_t *tmp = current_system->serialize(current_system, &actual_size);
	if (actual_size > size) {
		free(tmp);
		return 0;
	}
	memcpy(data, tmp, actual_size);
	free(tmp);
	return 1;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
	current_system->deserialize(current_system, (uint8_t *)data, size);
	return 0;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

/* Loads a game. */
RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	serialize_size_cache = 0;
	if (game->path) {
		media.dir = path_dirname(game->path);
		media.name = basename_no_extension(game->path);
		media.extension = path_extension(game->path);
	}
	media.buffer = malloc(nearest_pow2(game->size));
	memcpy(media.buffer, game->data, game->size);
	media.size = game->size;
	current_system = alloc_config_system(detect_system_type(&media), &media, 0, 0);
	
	unsigned format = RETRO_PIXEL_FORMAT_XRGB8888;
	retro_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);
	return current_system != NULL;
}

/* Loads a "special" kind of game. Should not be used,
 * except in extreme cases. */
RETRO_API bool retro_load_game_special(unsigned game_type, const struct retro_game_info *info, size_t num_info)
{
	return retro_load_game(info);
}

/* Unloads a currently loaded game. */
RETRO_API void retro_unload_game(void)
{
	
	free(media.dir);
	free(media.name);
	free(media.extension);
	
}

/* Gets region of game. */
RETRO_API unsigned retro_get_region(void)
{
	return video_standard == VID_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

/* Gets region of memory. */
RETRO_API void *retro_get_memory_data(unsigned id)
{
	return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	return 0;
}

//blastem render backend API implementation
uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return r << 16 | g << 8 | b;
}

uint8_t render_create_window(char *caption, uint32_t width, uint32_t height, window_close_handler close_handler)
{
	//not supported in lib build
	return 0;
}

void render_destroy_window(uint8_t which)
{
	//not supported in lib build
}

static uint32_t fb[LINEBUF_SIZE * 294 * 2];
uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
	*pitch = LINEBUF_SIZE * sizeof(uint32_t);
	//TODO: deal with interlace
	return fb;
}

void render_framebuffer_updated(uint8_t which, int width)
{
	//TODO: Deal with 256 px wide modes
	//TODO: deal with interlace
	retro_video_refresh(fb, LINEBUF_SIZE, video_standard == VID_NTSC ? 243 : 294, LINEBUF_SIZE * sizeof(uint32_t));
	current_system->request_exit(current_system);
}

uint8_t render_get_active_framebuffer(void)
{
	return 0;
}

void render_set_video_standard(vid_std std)
{
	video_standard = std;
}

void process_events()
{
	static int16_t prev_state[2][RETRO_DEVICE_ID_JOYPAD_L2];
	static const uint8_t map[] = {
		BUTTON_A, BUTTON_X, BUTTON_MODE, BUTTON_START, DPAD_UP, DPAD_DOWN,
		DPAD_LEFT, DPAD_RIGHT, BUTTON_B, BUTTON_Y, BUTTON_Z, BUTTON_C
	};
	//TODO: handle other input device types
	//TODO: handle more than 2 ports when appropriate
	retro_input_poll();
	for (int port = 0; port < 2; port++)
	{
		for (int id = RETRO_DEVICE_ID_JOYPAD_B; id < RETRO_DEVICE_ID_JOYPAD_L2; id++)
		{
			int16_t new_state = retro_input_state(port, RETRO_DEVICE_JOYPAD, 0, id);
			if (new_state != prev_state[port][id]) {
				if (new_state) {
					current_system->gamepad_down(current_system, port + 1, map[id]);
				} else {
					current_system->gamepad_up(current_system, port + 1, map[id]);
				}
				prev_state[port][id] = new_state;
			}
		}
	}
}

void render_errorbox(char *title, char *message)
{
}
void render_warnbox(char *title, char *message)
{
}
void render_infobox(char *title, char *message)
{
}

struct audio_source {
	int32_t freq;
	int32_t left_accum;
	int32_t right_accum;
	int32_t num_samples;
};

static audio_source *audio_sources[8];
static uint8_t num_audio_sources;
audio_source *render_audio_source(uint64_t master_clock, uint64_t sample_divider, uint8_t channels)
{
	audio_sources[num_audio_sources] = calloc(1, sizeof(audio_source));
	audio_sources[num_audio_sources]->freq = master_clock / sample_divider;
	return audio_sources[num_audio_sources++];
}

void render_audio_adjust_clock(audio_source *src, uint64_t master_clock, uint64_t sample_divider)
{
}

static void check_put_sample(void)
{
	for (int i = 0; i < num_audio_sources; i++)
	{
		int32_t min_samples = audio_sources[i]->freq / 53267;
		if (audio_sources[i]->num_samples < min_samples) {
			return;
		}
	}
	int16_t left = 0, right = 0;
	for (int i = 0; i < num_audio_sources; i++)
	{
		left += audio_sources[i]->left_accum / audio_sources[i]->num_samples;
		right += audio_sources[i]->right_accum / audio_sources[i]->num_samples;
		audio_sources[i]->left_accum = audio_sources[i]->right_accum = audio_sources[i]->num_samples = 0;
	}
	retro_audio_sample(left, right);
}

void render_put_mono_sample(audio_source *src, int16_t value)
{
	src->left_accum += value;
	src->right_accum += value;
	src->num_samples++;
	check_put_sample();
}
void render_put_stereo_sample(audio_source *src, int16_t left, int16_t right)
{
	src->left_accum += left;
	src->right_accum += right;
	src->num_samples++;
	check_put_sample();
}

void render_free_source(audio_source *src)
{
	int index;
	for (index = 0; index < num_audio_sources; index++)
	{
		if (audio_sources[index] == src) {
			break;
		}
	}
	num_audio_sources--;
	audio_sources[index] = audio_sources[num_audio_sources];
	free(src);
}

void bindings_set_mouse_mode(uint8_t mode)
{
}
