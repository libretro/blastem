#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "libretro.h"
#include "system.h"
#include "util.h"
#include "paths.h"
#include "vdp.h"
#include "render.h"
#include "io.h"
#include "genesis.h"
#include "sms.h"
#include "cdimage.h"
#include "vfs_file.h"

tern_node *config;

//The machines the standalone build can force with "-m". Detection handles most
//of them from the media itself, but it cannot tell a LaserActive disc from any
//other Sega CD one, and header based guesses are wrong often enough on Pico and
//Copera titles to be worth overriding.
//
//"jag" is deliberately missing: alloc_config_system() has no case for
//SYSTEM_JAGUAR and returns NULL, so offering it could only ever fail the load.
//"laseractive" keeps the spelling it shipped with, because frontends persist
//the chosen value in their config.
static const struct {
	const char *value;
	system_type stype;
} system_type_options[] = {
	{ "gen",         SYSTEM_GENESIS },
	{ "sms",         SYSTEM_SMS },
	{ "gg",          SYSTEM_GAME_GEAR },
	{ "sg1000",      SYSTEM_SG1000 },
	{ "sc3000",      SYSTEM_SC3000 },
	{ "32x",         SYSTEM_32X },
	{ "32xcd",       SYSTEM_32XCD },
	{ "pico",        SYSTEM_PICO },
	{ "copera",      SYSTEM_COPERA },
	{ "laseractive", SYSTEM_LASERACTIVE },
	{ "mediaplayer", SYSTEM_MEDIA_PLAYER }
};
#define NUM_SYSTEM_TYPE_OPTIONS (sizeof(system_type_options)/sizeof(*system_type_options))

static retro_environment_t retro_environment;
RETRO_API void retro_set_environment(retro_environment_t re)
{
	retro_environment = re;
#	define input_descriptor_macro(pad_num) \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_LEFT,  "D-Pad Left" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_UP,    "D-Pad Up" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_DOWN,  "D-Pad Down" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_RIGHT, "D-Pad Right" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_B,     "A" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_A,     "B" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_X,     "Y" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_Y,     "X" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_L,     "Z" }, \
		{ pad_num, RETRO_DEVICE_JOYPAD, 0, RETRO_DEVICE_ID_JOYPAD_R,     "C" }, \
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
		{0},
	};

	re(RETRO_ENVIRONMENT_SET_INPUT_DESCRIPTORS, (void *)desc);
	
	//NOTE: "smd" is deliberately absent here. Interleaved Super Magic Drive ROMs are
	//unscrambled by load_media(), which only runs on the need_fullpath path. Adding smd
	//would make the frontend hand us the raw interleaved buffer instead and load garbage.
	static const struct retro_system_content_info_override scio[] = {
		{
			.extensions = "md|gen|32x|sms|gg|sg|sg1|sc|sc3|sf7|col|vgm|flac|wav|bin|rom",
			.need_fullpath = 0,
			.persistent_data = 0
		},
		{0}
	};
	re(RETRO_ENVIRONMENT_SET_CONTENT_INFO_OVERRIDE, (void *)scio);

	//Built from the table rather than spelled out so the advertised values and the
	//ones option_system_type() accepts cannot drift apart. Rebuilt from scratch on
	//every call because a frontend may set the environment callback more than once.
	static char system_type_desc[256];
	int len = snprintf(system_type_desc, sizeof(system_type_desc), "System type; auto");
	for (size_t i = 0; i < NUM_SYSTEM_TYPE_OPTIONS && len < (int)sizeof(system_type_desc); i++) {
		len += snprintf(system_type_desc + len, sizeof(system_type_desc) - len, "|%s", system_type_options[i].value);
	}
	static struct retro_variable vars[] = {
		{ "blastem_system_type", NULL },
		{ NULL, NULL }
	};
	vars[0].value = system_type_desc;
	re(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);

	//Everything we open by name - CD images and each track file a cue sheet
	//points at, compressed ROMs, the Sega CD and 32X BIOS - goes through the
	//frontend when it offers this, so those paths work when they are Android
	//content:// URIs or live on an SMB share. Version 1 is all we need: open,
	//read, seek, tell, size and close. Nothing happens if it is unavailable,
	//vfs_file falls back to stdio.
	struct retro_vfs_interface_info vfs_info = { .required_interface_version = 1, .iface = NULL };
	if (re(RETRO_ENVIRONMENT_GET_VFS_INTERFACE, &vfs_info) && vfs_info.iface) {
		vfs_file_set_interface(vfs_info.iface, vfs_info.required_interface_version);
	}

	const char *system_dir = NULL;
	re(RETRO_ENVIRONMENT_GET_SYSTEM_DIRECTORY, &system_dir);
	printf("system_dir: %s\n", system_dir);
	if (system_dir) {
		config = tern_insert_path(config, "system\0scd_bios_us\0", (tern_val){.ptrval = alloc_concat(system_dir, "/bios_CD_U.bin")}, TVAL_PTR);
		config = tern_insert_path(config, "system\0scd_bios_eu\0", (tern_val){.ptrval = alloc_concat(system_dir, "/bios_CD_E.bin")}, TVAL_PTR);
		config = tern_insert_path(config, "system\0scd_bios_jp\0", (tern_val){.ptrval = alloc_concat(system_dir, "/bios_CD_J.bin")}, TVAL_PTR);
		config = tern_insert_path(config, "system\0coleco_bios_path\0", (tern_val){.ptrval = alloc_concat(system_dir, "/colecovision.rom")}, TVAL_PTR);
		//32x.c defaults these to bare filenames, which fopen() resolves against the
		//working directory - the frontend's, never the system directory - so the
		//32X BIOS ROMs were unreachable no matter where the user put them.
		config = tern_insert_path(config, "system\0s32x_68k_bios\0", (tern_val){.ptrval = alloc_concat(system_dir, "/32X_G_BIOS.bin")}, TVAL_PTR);
		config = tern_insert_path(config, "system\0s32x_main_bios\0", (tern_val){.ptrval = alloc_concat(system_dir, "/32X_M_BIOS.bin")}, TVAL_PTR);
		config = tern_insert_path(config, "system\0s32x_sub_bios\0", (tern_val){.ptrval = alloc_concat(system_dir, "/32X_S_BIOS.bin")}, TVAL_PTR);
		//Without an absolute path here alloc_laseractive() falls back to read_bundled_file(),
		//which looks next to the standalone binary and finds nothing in a libretro build.
		config = tern_insert_path(config, "system\0laseractive_upd_rom\0", (tern_val){.ptrval = alloc_concat(system_dir, "/laseractive_dyw_1322a.bin")}, TVAL_PTR);
	}
}

static retro_video_refresh_t retro_video_refresh;
RETRO_API void retro_set_video_refresh(retro_video_refresh_t rvf)
{
	retro_video_refresh = rvf;
}

RETRO_API void retro_set_audio_sample(retro_audio_sample_t ras)
{
}

static retro_audio_sample_batch_t retro_audio_sample_batch;
RETRO_API void retro_set_audio_sample_batch(retro_audio_sample_batch_t rasb)
{
	retro_audio_sample_batch = rasb;
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
uint8_t use_native_states = 1;
system_header *current_system;
static system_media media;
const system_media *current_media(void)
{
	return &media;
}

RETRO_API void retro_init(void)
{
	render_audio_initialized(RENDER_AUDIO_S16, 53693175 / (7 * 6 * 4), 2, 4, sizeof(int16_t));
}

RETRO_API void retro_deinit(void)
{
	if (current_system) {
		retro_unload_game();
	}
}

RETRO_API unsigned retro_api_version(void)
{
	return RETRO_API_VERSION;
}

#include "version.inc"

RETRO_API void retro_get_system_info(struct retro_system_info *info)
{
	info->library_name = "BlastEm";
	info->library_version = BLASTEM_VERSION;
	//gz and vgz are absent from the content info override for the same reason as
	//smd: they only decompress on the need_fullpath path, where romopen() is a
	//gzopen(). Handed over as data they would be loaded as raw deflate streams.
	info->valid_extensions = "md|gen|smd|32x|sms|gg|sg|sg1|sc|sc3|sf7|col|cue|toc|iso|chd|vgm|vgz|flac|wav|bin|rom|gz";
	info->need_fullpath = 1;
	info->block_extract = 0;
}

static vid_std video_standard;
static uint32_t last_width, last_height;
static uint8_t frame_presented;
static uint32_t overscan_top, overscan_bot, overscan_left, overscan_right;
static void update_overscan(void)
{
	uint8_t overscan;
	retro_environment(RETRO_ENVIRONMENT_GET_OVERSCAN, &overscan);
	if (overscan) {
		overscan_top = overscan_bot = overscan_left = overscan_right = 0;
	} else {
		if (video_standard == VID_NTSC) {
			overscan_top = 11;
			overscan_bot = 8;
			overscan_left = 13;
			overscan_right = 14;
		} else {
			overscan_top = 30;
			overscan_bot = 24;
			overscan_left = 13;
			overscan_right = 14;
		}
	}
}

static int32_t sample_rate;
RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	update_overscan();
	last_width = LINEBUF_SIZE;
	info->geometry.base_width = info->geometry.max_width = LINEBUF_SIZE - (overscan_left + overscan_right);
	info->geometry.base_height = (video_standard == VID_NTSC ? 243 : 294) - (overscan_top + overscan_bot);
	last_height = info->geometry.base_height;
	info->geometry.max_height = info->geometry.base_height * 2;
	info->geometry.aspect_ratio = 0;
	double master_clock = video_standard == VID_NTSC ? 53693175 : 53203395;
	double lines = video_standard == VID_NTSC ? 262 : 313;
	info->timing.fps = master_clock / (3420.0 * lines);
	info->timing.sample_rate = master_clock / (7 * 6 * 24); //sample rate of YM2612
	sample_rate = info->timing.sample_rate;
	render_audio_initialized(RENDER_AUDIO_S16, info->timing.sample_rate, 2, 4, sizeof(int16_t));
	//force adjustment of resampling parameters since target sample rate may have changed slightly
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
	frame_presented = 0;
	if (started) {
		current_system->resume_context(current_system);
	} else {
		current_system->start_context(current_system, NULL);
		started = 1;
	}
	//The media player has no video, so it returns without presenting anything.
	//Hand the frontend the (blank) framebuffer anyway so it still gets one frame
	//per call and its pacing and audio sync have something to run against.
	if (!frame_presented) {
		render_framebuffer_updated(render_get_active_framebuffer(), LINEBUF_SIZE);
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
		//VDP serialization size can vary based on FIFO fullness
		//add a little fudge factor here to ensure the returned size is always >= the actual size
		serialize_size_cache += 64;
		//We need to store the actual size saved too
		serialize_size_cache += sizeof(size_t);
	}
	return serialize_size_cache;
}

/* Serializes internal state. If failed, or size is lower than
 * retro_serialize_size(), it should return false, true otherwise. */
RETRO_API bool retro_serialize(void *data, size_t size)
{
	size_t *buffer = data;
	uint8_t *tmp = current_system->serialize(current_system, buffer);
	if (*buffer > size) {
		fprintf(stderr, "retro_serialize failed frontend size %d, actual size %d\n", (int)size, (int)*buffer);
		free(tmp);
		return 0;
	}
	memcpy(buffer + 1, tmp, *buffer);
	free(tmp);
	return 1;
}

RETRO_API bool retro_unserialize(const void *data, size_t size)
{
	const size_t *buffer = data;
	current_system->deserialize(current_system, (uint8_t *)(buffer + 1), *buffer);
	return 1;
}

RETRO_API void retro_cheat_reset(void)
{
}

RETRO_API void retro_cheat_set(unsigned index, bool enabled, const char *code)
{
}

static system_type option_system_type(void)
{
	struct retro_variable var = { .key = "blastem_system_type" };
	if (!retro_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var) || !var.value) {
		return SYSTEM_UNKNOWN;
	}
	for (size_t i = 0; i < NUM_SYSTEM_TYPE_OPTIONS; i++) {
		if (!strcmp(var.value, system_type_options[i].value)) {
			return system_type_options[i].stype;
		}
	}
	return SYSTEM_UNKNOWN;
}

//Disc formats blastem has no reader for. They are absent from valid_extensions,
//but a frontend can still hand one over - RetroArch loads whatever the user
//picks once "Filter Unknown Extensions" is off, and playlists carry paths the
//browser never filtered. Nothing downstream says no: load_media() has no case
//for them, so the generic path reads the entire image into memory - several
//hundred megabytes for a CD rip - and detect_system_type() then finds a "valid
//looking 68K reset vector" in whatever header it landed on and calls it a
//Genesis ROM. The frontend sits frozen for the length of the load and then runs
//garbage.
static uint8_t is_unsupported_disc_format(const char *ext)
{
	static const char *unsupported[] = { "ccd", "mds", "mdf", "nrg" };
	if (!ext) {
		return 0;
	}
	for (size_t i = 0; i < sizeof(unsupported)/sizeof(*unsupported); i++) {
		if (!strcasecmp(ext, unsupported[i])) {
			return 1;
		}
	}
	return 0;
}

//No cartridge comes close to this - the largest Mega Drive ROMs are 8 MB - so
//anything bigger is a disc image or a rip that detection has misread as a
//cartridge, and running it would only waste the memory it was read into.
#define MAX_CART_SIZE (64 * 1024 * 1024)
static uint8_t is_cartridge_system(system_type stype)
{
	switch (stype)
	{
	case SYSTEM_GENESIS:
	case SYSTEM_SMS:
	case SYSTEM_GAME_GEAR:
	case SYSTEM_SG1000:
	case SYSTEM_SC3000:
	case SYSTEM_COLECOVISION:
	case SYSTEM_PICO:
	case SYSTEM_COPERA:
	case SYSTEM_32X:
		return 1;
	default:
		return 0;
	}
}

//Building a system can hit fatal_error() - a missing Sega CD BIOS is the common
//one - and in a library that means exit(), i.e. the frontend disappears without
//so much as a message. Give it somewhere to land: a fatal error while a load is
//in progress unwinds to retro_load_game(), which reports the failure the way a
//frontend expects. Anything allocated by the half-built system is lost, which
//beats taking the process with it.
static jmp_buf fatal_recover;
static uint8_t fatal_recover_valid;
void lib_fatal_error(void)
{
	if (fatal_recover_valid) {
		fatal_recover_valid = 0;
		longjmp(fatal_recover, 1);
	}
}

//The SH2 BIOS ROMs are not optional: without them the SH2s execute zeroes and
//blastem stops at an unimplemented instruction, which is a fatal_error() from
//inside retro_run() where there is nothing to unwind. Missing firmware is an
//ordinary condition for a frontend, so refuse the load and say what is missing.
//An empty placeholder file counts as missing: the SH2 vector table lives at
//offset 0 and a reset vector of 0 is exactly what lands the emulator on the
//unimplemented instruction at pc=0. Any real BIOS has a non-zero one.
static uint8_t sh2_bios_usable(const char *key, const char *fallback)
{
	char *path = tern_find_path_default(config, key, (tern_val){.ptrval = (char *)fallback}, TVAL_PTR).ptrval;
	FILE *f = fopen(path, "rb");
	if (!f) {
		return 0;
	}
	uint8_t reset_vector[4] = {0};
	size_t got = fread(reset_vector, 1, sizeof(reset_vector), f);
	fclose(f);
	if (got != sizeof(reset_vector)) {
		return 0;
	}
	for (size_t i = 0; i < sizeof(reset_vector); i++) {
		if (reset_vector[i]) {
			return 1;
		}
	}
	return 0;
}

//For a load that fails no system context ever takes ownership of the media, and
//retro_unload_game() is not called either, so the buffer it left behind has to
//be released here - it is the whole file, which is exactly the case worth not
//leaking.
static void release_media(void)
{
	free(media.dir);
	free(media.name);
	free(media.extension);
	aligned_free(media.buffer);
	memset(&media, 0, sizeof(media));
}

/* Loads a game. */
static system_type stype;
RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	serialize_size_cache = 0;
	stype = SYSTEM_UNKNOWN;
	if (setjmp(fatal_recover)) {
		release_media();
		current_system = NULL;
		return 0;
	}
	fatal_recover_valid = 1;
	if (game->path) {
		char *ext = path_extension(game->path);
		uint8_t unsupported = is_unsupported_disc_format(ext);
		free(ext);
		if (unsupported) {
			warning("blastem cannot read this disc image format, use a cue/toc sheet or an iso instead\n");
			return 0;
		}
	}
	if (game->data) {
		if (game->path) {
			media.dir = path_dirname(game->path);
			media.name = basename_no_extension(game->path);
			media.extension = path_extension(game->path);
		}
		//the buffer is freed with aligned_free() by the system context, so it
		//needs the alignment header that aligned_calloc() puts in front of it
		media.buffer = aligned_calloc(1, nearest_pow2(game->size), 16);
		memcpy(media.buffer, game->data, game->size);
		media.size = game->size;
	} else {
		load_media((char *)game->path, &media, &stype);
	}
	//Same precedence as blastem.c: an explicit choice wins over whatever load_media()
	//worked out, and detection only runs when nothing has been decided yet.
	system_type force_stype = option_system_type();
	if (force_stype != SYSTEM_UNKNOWN) {
		stype = force_stype;
	}
	if (stype == SYSTEM_UNKNOWN) {
		stype = detect_system_type(&media);
	}
	if (is_cartridge_system(stype) && media.size > MAX_CART_SIZE) {
		warning("%u byte image was detected as a cartridge, refusing to load it\n", (unsigned)media.size);
		release_media();
		fatal_recover_valid = 0;
		return 0;
	}
	if ((stype == SYSTEM_32X || stype == SYSTEM_32XCD)
		&& !(sh2_bios_usable("system\0s32x_main_bios\0", "32X_M_BIOS.bin")
			&& sh2_bios_usable("system\0s32x_sub_bios\0", "32X_S_BIOS.bin"))) {
		warning("32X needs the Main and Sub SH2 BIOS ROMs (32X_M_BIOS.bin and 32X_S_BIOS.bin) in the system directory\n");
		release_media();
		fatal_recover_valid = 0;
		return 0;
	}
	current_system = alloc_config_system(stype, &media, 0, 0);
	fatal_recover_valid = 0;
	if (!current_system) {
		release_media();
		return 0;
	}

	unsigned format = RETRO_PIXEL_FORMAT_XRGB8888;
	retro_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

	return 1;
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
	media.dir = media.name = media.extension = NULL;
	//buffer is freed by the context
	media.buffer = NULL;
	current_system->free_context(current_system);
	current_system = NULL;
	//the next system loaded has never run, so retro_run() must start it rather
	//than resume it; resuming would enter the recompiler at a NULL resume_pc
	started = 0;
}

/* Gets region of game. */
RETRO_API unsigned retro_get_region(void)
{
	return video_standard == VID_NTSC ? RETRO_REGION_NTSC : RETRO_REGION_PAL;
}

/* Gets region of memory. */
RETRO_API void *retro_get_memory_data(unsigned id)
{
	switch (id) {
	case RETRO_MEMORY_SYSTEM_RAM:
		switch (stype) {
		case SYSTEM_GENESIS: {
			genesis_context *gen = (genesis_context *)current_system;
			return (uint8_t *)gen->work_ram;
		}
#ifndef NO_Z80
		case SYSTEM_SMS: {
			sms_context *sms = (sms_context *)current_system;
			return sms->ram;
		}
#endif
		}
		break;
	case RETRO_MEMORY_SAVE_RAM:
		if (stype == SYSTEM_GENESIS) {
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
	switch (id) {
	case RETRO_MEMORY_SYSTEM_RAM:
		switch (stype) {
		case SYSTEM_GENESIS:
			return RAM_WORDS * sizeof(uint16_t);
#ifndef NO_Z80
		case SYSTEM_SMS:
			return SMS_RAM_SIZE;
#endif
		}
		break;
	case RETRO_MEMORY_SAVE_RAM:
		if (stype == SYSTEM_GENESIS) {
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
static uint8_t last_fb;
uint32_t *render_get_framebuffer(uint8_t which, int *pitch)
{
	*pitch = LINEBUF_SIZE * sizeof(uint32_t);
	if (which != last_fb) {
		*pitch = *pitch * 2;
	}

	if (which) {
		return fb + LINEBUF_SIZE;
	} else {
		return fb;
	}
}

void render_framebuffer_updated(uint8_t which, int width)
{
	unsigned height = (video_standard == VID_NTSC ? 243 : 294) - (overscan_top + overscan_bot);
	width -= (overscan_left + overscan_right);
	unsigned base_height = height;
	if (which != last_fb) {
		height *= 2;
		last_fb = which;
	}
	if (width != last_width || height != last_height) {
		struct retro_game_geometry geometry = {
			.base_width = width,
			.base_height = height,
			.aspect_ratio = (float)LINEBUF_SIZE / base_height
		};
		retro_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
		last_width = width;
		last_height = height;
	}
	retro_video_refresh(fb + overscan_left + LINEBUF_SIZE * overscan_top, width, height, LINEBUF_SIZE * sizeof(uint32_t));
	frame_presented = 1;
	system_request_exit(current_system, 0);
}

uint8_t render_get_active_framebuffer(void)
{
	return 0;
}

void render_set_video_standard(vid_std std)
{
	video_standard = std;
}

uint8_t render_fullscreen(void)
{
	return 1;
}

uint32_t render_overscan_top()
{
	return overscan_top;
}

uint32_t render_overscan_bot()
{
	return overscan_bot;
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

uint8_t render_is_audio_sync(void)
{
	//whether this is true depends on the libretro frontend implementation
	//but the sync to audio path works better here
	return 1;
}

uint8_t render_should_release_on_exit(void)
{
	return 0;
}

void render_buffer_consumed(audio_source *src)
{
}

void *render_new_audio_opaque(void)
{
	return NULL;
}

void render_free_audio_opaque(void *opaque)
{
}

void render_lock_audio(void)
{
}

void render_unlock_audio()
{
}

uint32_t render_min_buffered(void)
{
	//not actually used in the sync to audio path
	return 4;
}

uint32_t render_audio_syncs_per_sec(void)
{
	return 0;
}

void render_audio_created(audio_source *src)
{
}

void render_do_audio_ready(audio_source *src)
{
	int16_t *tmp = src->front;
	src->front = src->back;
	src->back = tmp;
	src->front_populated = 1;
	src->buffer_pos = 0;
	if (all_sources_ready()) {
		int16_t buffer[8];
		int min_remaining_out;
		mix_and_convert((uint8_t *)buffer, sizeof(buffer), &min_remaining_out);
		retro_audio_sample_batch(buffer, sizeof(buffer)/(2*sizeof(*buffer)));
	}
}

void render_source_paused(audio_source *src, uint8_t remaining_sources)
{
}

void render_source_resumed(audio_source *src)
{
}

void render_set_external_sync(uint8_t ext_sync_on)
{
}

char * render_read_clipboard(void)
{
	return NULL;
}

void bindings_set_mouse_mode(uint8_t mode)
{
}

void bindings_release_capture(void)
{
}

void bindings_reacquire_capture(void)
{
}

extern const char rom_db_data[];
char *read_bundled_file(char *name, uint32_t *sizeret)
{
	if (!strcmp(name, "rom.db")) {
		*sizeret = strlen(rom_db_data);
		char *ret = malloc(*sizeret+1);
		memcpy(ret, rom_db_data, *sizeret + 1);
		return ret;
	}
	return NULL;
}
