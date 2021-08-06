#include <stdlib.h>
#include <string.h>
#include "libretro.h"
#include "system.h"
#include "util.h"
#include "vdp.h"
#include "render.h"
#include "io.h"
#include "genesis.h"
#include "sms.h"

static retro_environment_t retro_environment;
static retro_video_refresh_t retro_video_refresh;
static retro_audio_sample_batch_t retro_audio_sample_batch;
static retro_input_poll_t retro_input_poll;
static retro_input_state_t retro_input_state;

static bool libretro_supports_bitmasks    = false;

RETRO_API void retro_set_environment(retro_environment_t re)
{
	retro_environment = re;
#	define input_descriptor_macro(pad_num) \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "B" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "C" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "A" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "X" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "Z" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_SELECT,    "Mode" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_START,    "Start" }, \

	static const struct retro_input_descriptor desc[] = {
		input_descriptor_macro(0)
		input_descriptor_macro(1)
		input_descriptor_macro(2)
		input_descriptor_macro(3)
		input_descriptor_macro(4)
		input_descriptor_macro(5)
		input_descriptor_macro(6)
		input_descriptor_macro(7)
		{ 0 },
	};

	re(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
}

RETRO_API void retro_set_video_refresh(retro_video_refresh_t rvf)
{
	retro_video_refresh = rvf;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t ras) { }

RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t rasb)
{
	retro_audio_sample_batch = rasb;
}

RETRO_API void retro_set_input_poll(retro_input_poll_t rip)
{
	retro_input_poll = rip;
}

RETRO_API void retro_set_input_state(retro_input_state_t ris)
{
	retro_input_state = ris;
}

char *save_filename           = NULL;
tern_node *config             = NULL;
int headless                  = 0;
int exit_after                = 0;
int z80_enabled               = 1;
uint8_t use_native_states     = 1;
system_header *current_system = NULL;
static system_media current_media;

RETRO_API void retro_init(void)
{
	render_audio_initialized(RENDER_AUDIO_S16,
         53693175 / (7 * 6 * 4), 2, 4, sizeof(int16_t));

   libretro_supports_bitmasks = false;
   if (retro_environment(RETRO_ENVIRONMENT_GET_INPUT_BITMASKS, NULL))
      libretro_supports_bitmasks = true;
}

RETRO_API void retro_deinit(void)
{
	if (current_system)
		retro_unload_game();
   libretro_supports_bitmasks = false;
}

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	info->library_name     = "BlastEm";
	info->library_version  = "0.6.3-pre"; /* TODO: share this with blastem.c */
	info->valid_extensions = "md|gen|sms|bin|rom";
	info->need_fullpath    = false;
	info->block_extract    = false;
}

static vid_std video_standard;
static uint32_t last_width, last_height;
static uint32_t overscan_top, overscan_bot, overscan_left, overscan_right;

static void update_overscan(void)
{
	uint8_t overscan;
	retro_environment(RETRO_ENVIRONMENT_GET_OVERSCAN, &overscan);
	if (overscan)
   {
      overscan_top      = 0;
      overscan_bot      = 0;
      overscan_left     = 0;
      overscan_right    = 0;
   }
	else
   {
      if (video_standard == VID_NTSC)
      {
         overscan_top   = 11;
         overscan_bot   = 8;
         overscan_left  = 13;
         overscan_right = 14;
      }
      else
      {
         overscan_top   = 30;
         overscan_bot   = 24;
         overscan_left  = 13;
         overscan_right = 14;
      }
   }
}

static float get_aspect_ratio(void)
{
	float aspect_width  = LINEBUF_SIZE - overscan_left - overscan_right;
	float aspect_height = (video_standard == VID_NTSC ? 243 : 294) 
      - overscan_top - overscan_bot;
	return aspect_width / aspect_height;
}

static int32_t sample_rate;

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
   double master_clock, lines;

	update_overscan();
	last_width                  = LINEBUF_SIZE;
	info->geometry.base_width   = info->geometry.max_width = LINEBUF_SIZE - (overscan_left + overscan_right);
	info->geometry.base_height  = (video_standard == VID_NTSC ? 243 : 294) - (overscan_top + overscan_bot);
	last_height                 = info->geometry.base_height;
	info->geometry.max_height   = info->geometry.base_height * 2;
	info->geometry.aspect_ratio = get_aspect_ratio();
	master_clock                = video_standard == VID_NTSC ? 53693175 : 53203395;
	lines                       = video_standard == VID_NTSC ? 262 : 313;
	info->timing.fps            = master_clock / (3420.0 * lines);
	info->timing.sample_rate    = master_clock / (7 * 6 * 24); //sample rate of YM2612
	sample_rate                 = info->timing.sample_rate;
	render_audio_initialized(RENDER_AUDIO_S16,
         info->timing.sample_rate, 2, 4, sizeof(int16_t));
	/* Force adjustment of resampling parameters 
    * since target sample rate may have changed slightly */
	current_system->set_speed_percent(current_system, 100);
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
	retro_input_poll();
	if (started)
		current_system->resume_context(current_system);
	else
	{
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
RETRO_API size_t retro_serialize_size(void)
{
	return SERIALIZE_DEFAULT_SIZE;
}

/* Serializes internal state. If failed, or size is lower than
 * retro_serialize_size(), it should return false, true otherwise. */
RETRO_API bool retro_serialize(void *data, size_t size)
{
	size_t actual_size;
	uint8_t *tmp = current_system->serialize(current_system, &actual_size);
	if (actual_size > size)
   {
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
	return 1;
}

RETRO_API void retro_cheat_reset(void) { }
RETRO_API void retro_cheat_set(unsigned index,
      bool enabled, const char *code)  { }

/* Loads a game. */
static system_type stype;
RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	unsigned format = RETRO_PIXEL_FORMAT_XRGB8888;

	if (game->path)
   {
		current_media.dir       = path_dirname(game->path);
		current_media.name      = basename_no_extension(game->path);
		current_media.extension = path_extension(game->path);
	}

   current_media.buffer       = malloc(nearest_pow2(game->size));
   memcpy(current_media.buffer, game->data, game->size);
   current_media.size         = game->size;
   stype              = detect_system_type(&current_media);
   current_system     = alloc_config_system(stype, &current_media, 0, 0);

   retro_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

   if (!current_system)
      return false;
   return true;
}

/* Loads a "special" kind of game. Should not be used,
 * except in extreme cases. */
RETRO_API bool retro_load_game_special(
      unsigned game_type, const struct retro_game_info *info,
      size_t num_info)
{
	return retro_load_game(info);
}

/* Unloads a currently loaded game. */
RETRO_API void retro_unload_game(void)
{
	free(current_media.dir);
	free(current_media.name);
	free(current_media.extension);
	current_media.dir       = NULL;
   current_media.name      = NULL;
   current_media.extension = NULL;
	/* buffer is freed by the context */
	current_media.buffer    = NULL;

	current_system->free_context(current_system);
	current_system  = NULL;
}

/* Gets region of game. */
RETRO_API unsigned retro_get_region(void)
{
	return video_standard == VID_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

/* Gets region of memory. */
RETRO_API void *retro_get_memory_data(unsigned id)
{
	switch (id)
   {
      case RETRO_MEMORY_SYSTEM_RAM:
         switch (stype)
         {
            case SYSTEM_GENESIS:
               {
                  genesis_context *gen = (genesis_context *)current_system;
                  return (uint8_t *)gen->work_ram;
               }
#ifndef NO_Z80
            case SYSTEM_SMS:
               {
                  sms_context *sms = (sms_context *)current_system;
                  return sms->ram;
               }
#endif
         }
         break;
      case RETRO_MEMORY_SAVE_RAM:
         if (stype == SYSTEM_GENESIS)
         {
            genesis_context *gen = (genesis_context *)current_system;
            if (gen->save_type != SAVE_NONE)
               return gen->save_storage;
         }
         break;
      default:
         break;
   }
	return NULL;
}

RETRO_API size_t retro_get_memory_size(unsigned id)
{
	switch (id)
   {
      case RETRO_MEMORY_SYSTEM_RAM:
         switch (stype)
         {
            case SYSTEM_GENESIS:
               return RAM_WORDS * sizeof(uint16_t);
#ifndef NO_Z80
            case SYSTEM_SMS:
               return SMS_RAM_SIZE;
#endif
         }
         break;
      case RETRO_MEMORY_SAVE_RAM:
         if (stype == SYSTEM_GENESIS)
         {
            genesis_context *gen = (genesis_context *)current_system;
            if (gen->save_type != SAVE_NONE)
               return gen->save_size;
         }
         break;
      default:
         break;
   }
	return 0;
}

/* blastem render backend API implementation */
uint32_t render_map_color(uint8_t r, uint8_t g, uint8_t b)
{
	return r << 16 | g << 8 | b;
}

/* Not supported in lib build */
uint8_t render_create_window(char *caption,
      uint32_t width, uint32_t height, window_close_handler close_handler)
{
	return 0;
}

/* Not supported in lib build */
void render_destroy_window(uint8_t which) { }

static uint32_t fb[LINEBUF_SIZE * 294 * 2];
static uint8_t last_fb;

uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
   *pitch = LINEBUF_SIZE * sizeof(uint32_t);
   if (which != last_fb)
      *pitch = *pitch * 2;

   if (which)
      return fb + LINEBUF_SIZE;
   return fb;
}

void render_framebuffer_updated(uint8_t which, int width)
{
   unsigned base_height;
   unsigned height      = (video_standard == VID_NTSC ? 243 : 294) 
      - (overscan_top + overscan_bot);
   width               -= (overscan_left + overscan_right);
   base_height          = height;

   if (which != last_fb)
   {
      height           *= 2;
      last_fb           = which;
   }
   if (width != last_width || height != last_height)
   {
      struct retro_game_geometry geometry;
      geometry.base_width   = width;
      geometry.base_height  = height;
      geometry.aspect_ratio = get_aspect_ratio();
      retro_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
      last_width            = width;
      last_height           = height;
   }
   retro_video_refresh(fb + overscan_left + LINEBUF_SIZE * overscan_top, width, height, LINEBUF_SIZE * sizeof(uint32_t));
   system_request_exit(current_system, 0);
}

uint8_t render_get_active_framebuffer(void) { return 0; }
void render_set_video_standard(vid_std std) { video_standard = std; }
int render_fullscreen(void)                 { return 1; }
uint32_t render_overscan_top(void)          { return overscan_top; }
uint32_t render_overscan_bot(void)          { return overscan_bot; }

void process_events(void)
{
   int port;
   int16_t inputs[2];
	static int16_t prev_state[2][RETRO_DEVICE_ID_JOYPAD_L2];
	static const uint8_t map[] = {
		BUTTON_B, BUTTON_A, BUTTON_MODE, BUTTON_START, DPAD_UP, DPAD_DOWN,
		DPAD_LEFT, DPAD_RIGHT, BUTTON_C, BUTTON_Y, BUTTON_X, BUTTON_Z
	};

	/* TODO: handle other input device types
	 * TODO: handle more than 2 ports when appropriate
    */
	retro_input_poll();

   inputs[0]    = 0;
   inputs[1]    = 0;

   if (libretro_supports_bitmasks)
   {
      inputs[0] = retro_input_state(0, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
      inputs[1] = retro_input_state(1, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_MASK);
   }
   else
   {
      for (port = 0; port < 2; port++)
      {
         int id;
         for (id = RETRO_DEVICE_ID_JOYPAD_B; id < RETRO_DEVICE_ID_JOYPAD_L2; id++)
         {
            if (retro_input_state(port, RETRO_DEVICE_JOYPAD, 0, id))
               inputs[port] |= (1 << id);
         }
      }
   }

   for (port = 0; port < 2; port++)
   {
      int id;
      for (id = RETRO_DEVICE_ID_JOYPAD_B; id < RETRO_DEVICE_ID_JOYPAD_L2; id++)
      {
         int16_t new_state = inputs[port] & (1 << id);
         if (new_state != prev_state[port][id])
         {
            if (new_state)
               current_system->gamepad_down(current_system, port + 1, map[id]);
            else
               current_system->gamepad_up(current_system, port + 1, map[id]);
            prev_state[port][id] = new_state;
         }
      }
   }
}

void render_errorbox(char *title, char *message) { }
void render_warnbox(char *title, char *message)  { }
void render_infobox(char *title, char *message)  { }

/* Whether this is true depends on the 
 * libretro frontend implementation,
 * but the sync to audio path works better here */
uint8_t render_is_audio_sync(void)             { return 1; }
uint8_t render_should_release_on_exit(void)    { return 0; }
void render_buffer_consumed(audio_source *src) { }
void *render_new_audio_opaque(void)            { return NULL; }

void render_free_audio_opaque(void *opaque)    { }
void render_lock_audio(void)                   { }
void render_unlock_audio(void)                 { }
/*not actually used in the sync to audio path */
uint32_t render_min_buffered(void)             { return 4; }
uint32_t render_audio_syncs_per_sec(void)      { return 0; }
void render_audio_created(audio_source *src)   { }

void render_do_audio_ready(audio_source *src)
{
   int16_t *tmp         = src->front;
   src->front           = src->back;
   src->back            = tmp;
   src->front_populated = 1;
   src->buffer_pos      = 0;

   if (all_sources_ready())
   {
      int16_t buffer[8];
      int min_remaining_out;
      mix_and_convert((uint8_t *)buffer, sizeof(buffer), &min_remaining_out);
      retro_audio_sample_batch(buffer, sizeof(buffer)/(2*sizeof(*buffer)));
   }
}

void render_source_paused(audio_source *src, uint8_t remaining_sources) { }
void render_source_resumed(audio_source *src)                           { }
void render_set_external_sync(uint8_t ext_sync_on)                      { }
void bindings_set_mouse_mode(uint8_t mode)                              { }
void bindings_release_capture(void)                                     { }
void bindings_reacquire_capture(void)                                   { }

extern const char rom_db_data[];
char *read_bundled_file(char *name, uint32_t *sizeret)
{
	if (!strcmp(name, "rom.db"))
   {
      *sizeret  = strlen(rom_db_data);
      char *ret = malloc(*sizeret+1);
      memcpy(ret, rom_db_data, *sizeret + 1);
      return ret;
   }
	return NULL;
}
