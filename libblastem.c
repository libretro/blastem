#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <setjmp.h>
#include "libretro.h"
#include "system.h"
#include "util.h"
#include "config.h"
#include "paths.h"
#include "vdp.h"
#include "render.h"
#include "io.h"
#include "genesis.h"
#include "sms.h"
#include "cdimage.h"
#include "vfs_file.h"
#include "megawifi.h"

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

/*
 Core options.

 Everything the standalone build lets a user change lives in one config tree,
 which the emulation code reads with tern_find_path() as it builds a machine. A
 libretro frontend has its own settings UI, so each option below is one of those
 config keys wearing a different name, written back into the same tree before a
 system is created.

 What the standalone offers but is missing here belongs to the frontend rather
 than to the emulator: window size, shaders, scaling and vsync, audio device
 rate and buffer size, key bindings, sync source, save paths, the file picker.
 Sega CD, 32X, Colecovision and TMSS ROM paths are settings too, but a libretro
 core takes those from the system directory instead of asking.
*/
typedef struct {
	const char *key;
	const char *desc;
	const char *info;
	const char *category;
	//The config path the emulator reads it from, or NULL for an option this file
	//acts on itself.
	const char *config_path;
	//NULL terminated, first entry is the default. Points at one of the static
	//lists below, or at one filled in by build_dynamic_values() for the ones
	//whose choices are only known at runtime.
	const struct retro_core_option_value *values;
} core_option;

//Long lists that are built rather than spelled out, so that what is advertised
//and what the code accepts cannot drift apart.
#define MAX_MODEL_VALUES 32
#define MAX_OVERSCAN 32
static struct retro_core_option_value system_type_values[NUM_SYSTEM_TYPE_OPTIONS + 2];
static struct retro_core_option_value model_values[MAX_MODEL_VALUES + 1];
static struct retro_core_option_value sms_model_values[MAX_MODEL_VALUES + 1];
static struct retro_core_option_value overscan_values[MAX_OVERSCAN + 3];

static const struct retro_core_option_value region_values[] = {
	{ "U", "U - Americas" },
	{ "J", "J - Japan" },
	{ "E", "E - Europe" },
	{ NULL, NULL }
};
static const struct retro_core_option_value off_on_values[] = {
	{ "off", "Off" },
	{ "on", "On" },
	{ NULL, NULL }
};
static const struct retro_core_option_value ram_init_values[] = {
	{ "zero", "Zero" },
	{ "random", "Random" },
	{ NULL, NULL }
};
//The standalone lets any divider from 1 to 53 be typed in; these are the ones
//worth picking from a list. 7 is the hardware's.
static const struct retro_core_option_value m68k_divider_values[] = {
	{ "7", "7 (7.67 MHz, native)" },
	{ "6", "6 (8.9 MHz)" },
	{ "5", "5 (10.7 MHz)" },
	{ "4", "4 (13.4 MHz)" },
	{ "3", "3 (17.9 MHz)" },
	{ "2", "2 (26.8 MHz)" },
	{ "1", "1 (53.7 MHz)" },
	{ "8", "8 (6.7 MHz)" },
	{ "10", "10 (5.4 MHz)" },
	{ "14", "14 (3.8 MHz)" },
	{ NULL, NULL }
};
//Only the pad types the input glue below can actually drive. The standalone's
//list also has multitaps, mice and keyboards, which need ports and device types
//this core does not implement yet.
static const struct retro_core_option_value io_1_values[] = {
	{ "gamepad6.1", "6-button pad" },
	{ "gamepad3.1", "3-button pad" },
	{ "none", "None" },
	{ NULL, NULL }
};
static const struct retro_core_option_value io_2_values[] = {
	{ "gamepad6.2", "6-button pad" },
	{ "gamepad3.2", "3-button pad" },
	{ "none", "None" },
	{ NULL, NULL }
};
static const struct retro_core_option_value lowpass_values[] = {
	{ "3390", "3390 Hz (Model 1)" },
	{ "2000", "2000 Hz" },
	{ "2500", "2500 Hz" },
	{ "3000", "3000 Hz" },
	{ "4000", "4000 Hz" },
	{ "5000", "5000 Hz" },
	{ "8000", "8000 Hz" },
	{ "24000", "24000 Hz (off)" },
	{ NULL, NULL }
};
//Gain lists share everything but which entry comes first, since the standalone
//starts the mixed-in chips lower than the ones on the main board.
#define GAIN_VALUES_TAIL \
	{ "6.0", "+6 dB" }, \
	{ "4.5", "+4.5 dB" }, \
	{ "3.0", "+3 dB" }, \
	{ "1.5", "+1.5 dB" }, \
	{ "0.0", "0 dB" }, \
	{ "-1.5", "-1.5 dB" }, \
	{ "-3.0", "-3 dB" }, \
	{ "-4.5", "-4.5 dB" }, \
	{ "-6.0", "-6 dB" }, \
	{ "-9.5", "-9.5 dB" }, \
	{ "-12.0", "-12 dB" }, \
	{ "-18.0", "-18 dB" }, \
	{ "-24.0", "-24 dB" }, \
	{ NULL, NULL }
static const struct retro_core_option_value gain_values[] = {
	{ "0.0", "0 dB" },
	GAIN_VALUES_TAIL
};
static const struct retro_core_option_value rf5c164_gain_values[] = {
	{ "-6.0", "-6 dB" },
	GAIN_VALUES_TAIL
};
static const struct retro_core_option_value cdda_gain_values[] = {
	{ "-9.5", "-9.5 dB" },
	GAIN_VALUES_TAIL
};
static const struct retro_core_option_value fm_dac_values[] = {
	{ "auto", "Default for model" },
	{ "zero_offset", "Zero offset (discrete YM2612)" },
	{ "linear", "Linear (integrated YM3438)" },
	{ NULL, NULL }
};

static const core_option core_options[] = {
	{
		"blastem_system_type", "System Type",
		"Which machine to emulate. Auto works out the system from the media, which is right for everything except a LaserActive disc and some Pico and Copera titles.",
		"system", NULL, system_type_values
	},
	{
		"blastem_model", "Mega Drive Model",
		"Which revision of the console to emulate. Models from the Mega Drive 2 on have TMSS, which shows the licensed-by-Sega screen at boot and needs the TMSS ROM in the system directory, named tmss.md or bios_MD.bin.",
		"system", "system\0model\0", model_values
	},
	{
		"blastem_sms_model", "Master System Model",
		"Which machine to run Master System software on. The Mega Drive models run it the way a Mega Drive with a Power Base Converter does.",
		"system", "sms\0system\0model\0", sms_model_values
	},
	{
		"blastem_region", "Default Region",
		"Region to use when the software does not say which it wants, or says it will take more than one.",
		"system", "system\0default_region\0", region_values
	},
	{
		"blastem_force_region", "Force Default Region",
		"Use the region above even for software that asks for a different one.",
		"system", "system\0force_region\0", off_on_values
	},
	{
		"blastem_ram_init", "Initial RAM Value",
		"What work RAM holds at power on. Random exposes software that relies on uninitialized memory.",
		"system", "system\0ram_init\0", ram_init_values
	},
	{
		"blastem_m68k_divider", "68000 Clock Divider",
		"Divider between the master clock and the 68000. Anything below 7 overclocks the CPU, which some software will not survive.",
		"system", "clocks\0m68k_divider\0", m68k_divider_values
	},
	{
		"blastem_megawifi", "MegaWiFi",
		"Emulate the MegaWiFi cartridge hardware, which lets software make its own connections to the internet. Only turn this on for software you trust.",
		"system", "system\0megawifi\0", off_on_values
	},
	{
		"blastem_io_1", "Port 1 Device",
		"What is plugged into the first controller port.",
		"input", "io\0devices\0" "1\0", io_1_values
	},
	{
		"blastem_io_2", "Port 2 Device",
		"What is plugged into the second controller port.",
		"input", "io\0devices\0" "2\0", io_2_values
	},
	{
		"blastem_gain", "Overall Gain",
		"Gain applied to the mix of every sound chip.",
		"audio", "audio\0gain\0", gain_values
	},
	{
		"blastem_fm_gain", "FM Gain",
		"Gain applied to the YM2612 / YM3438.",
		"audio", "audio\0fm_gain\0", gain_values
	},
	{
		"blastem_psg_gain", "PSG Gain",
		"Gain applied to the SN76489 PSG.",
		"audio", "audio\0psg_gain\0", gain_values
	},
	{
		"blastem_rf5c164_gain", "RF5C164 Gain",
		"Gain applied to the Sega CD's PCM chip.",
		"audio", "audio\0rf5c164_gain\0", rf5c164_gain_values
	},
	{
		//The standalone's settings menu writes this one to "audio.cdd_gain",
		//which is not the key set_audio_config() reads, so the slider there does
		//nothing. Spelled the way the emulator reads it.
		"blastem_cdda_gain", "CD Audio Gain",
		"Gain applied to CD audio tracks.",
		"audio", "audio\0cdda_gain\0", cdda_gain_values
	},
	{
		"blastem_fm_dac", "FM DAC",
		"Which DAC behaviour to emulate. The discrete YM2612's zero offset is the source of the ladder effect, the integrated YM3438 in later models does not have it.",
		"audio", "audio\0fm_dac\0", fm_dac_values
	},
	{
		"blastem_lowpass_cutoff", "Low Pass Filter Cutoff",
		"Cutoff of the low pass filter the console's audio circuit applies. Higher values leave more high end in.",
		"audio", "audio\0lowpass_cutoff\0", lowpass_values
	},
	{
		"blastem_overscan_top", "Overscan Cropped, Top",
		"Lines cropped off the top of the picture. Auto crops the region's overscan area, which in some games also "
		"shaves a frame off the input lag: the picture is finished before the game reads the pad.",
		"video", NULL, overscan_values
	},
	{
		"blastem_overscan_bottom", "Overscan Cropped, Bottom",
		"Lines cropped off the bottom of the picture. Auto crops the region's overscan area.",
		"video", NULL, overscan_values
	},
	{
		"blastem_overscan_left", "Overscan Cropped, Left",
		"Columns cropped off the left of the picture. Auto crops the region's overscan area.",
		"video", NULL, overscan_values
	},
	{
		"blastem_overscan_right", "Overscan Cropped, Right",
		"Columns cropped off the right of the picture. Auto crops the region's overscan area.",
		"video", NULL, overscan_values
	}
};
#define NUM_CORE_OPTIONS (sizeof(core_options)/sizeof(*core_options))

//The model list is whatever systems.cfg holds, filtered the way the standalone's
//settings menu filters it: models that hide themselves are left out, and the
//Mega Drive list only keeps the ones with a Mega Drive VDP.
typedef struct {
	struct retro_core_option_value *values;
	uint32_t num;
	uint32_t max;
	uint8_t genesis_only;
} model_collect_state;

static void model_iter(char *key, tern_val val, uint8_t valtype, void *data)
{
	model_collect_state *state = data;
	if (valtype != TVAL_NODE || state->num == state->max) {
		return;
	}
	tern_node *model = val.ptrval;
	if (!strcmp(tern_find_ptr_default(model, "show", "yes"), "no")) {
		return;
	}
	if (state->genesis_only && strcmp(tern_find_ptr_default(model, "vdp", "genesis"), "genesis")) {
		return;
	}
	//tern_foreach() hands out one reusable buffer for the key, so it has to be
	//copied; the name it is displayed under lives in the config tree and does not.
	char *name = strdup(key);
	state->values[state->num].value = name;
	state->values[state->num].label = tern_find_ptr_default(model, "name", name);
	state->num++;
}

static void collect_models(struct retro_core_option_value *values, uint32_t max, uint8_t genesis_only, const char *first)
{
	model_collect_state state = { .values = values, .num = 0, .max = max, .genesis_only = genesis_only };
	tern_foreach(get_systems_config(), model_iter, &state);
	//The frontend takes the first entry as the default, so move it to the front.
	for (uint32_t i = 1; i < state.num; i++)
	{
		if (!strcmp(values[i].value, first)) {
			struct retro_core_option_value tmp = values[i];
			memmove(values + 1, values, i * sizeof(*values));
			values[0] = tmp;
			break;
		}
	}
	values[state.num].value = values[state.num].label = NULL;
}

static void build_dynamic_values(void)
{
	static uint8_t built;
	if (built) {
		return;
	}
	built = 1;
	system_type_values[0].value = "auto";
	system_type_values[0].label = "Auto";
	for (size_t i = 0; i < NUM_SYSTEM_TYPE_OPTIONS; i++)
	{
		system_type_values[i + 1].value = system_type_values[i + 1].label = system_type_options[i].value;
	}
	system_type_values[NUM_SYSTEM_TYPE_OPTIONS + 1].value = NULL;
	system_type_values[NUM_SYSTEM_TYPE_OPTIONS + 1].label = NULL;

	//md1va3 is what the standalone defaults to and what this core emulated before
	//it could be chosen. Master System software defaults to a real Master System
	//here rather than to the standalone's md1va3, which would run it through the
	//Mega Drive VDP - a change of behaviour rather than a new option.
	collect_models(model_values, MAX_MODEL_VALUES, 1, "md1va3");
	collect_models(sms_model_values, MAX_MODEL_VALUES, 0, "sms2");

	static char overscan_numbers[MAX_OVERSCAN + 1][3];
	overscan_values[0].value = "auto";
	overscan_values[0].label = "Auto";
	for (uint32_t i = 0; i <= MAX_OVERSCAN; i++)
	{
		snprintf(overscan_numbers[i], sizeof(overscan_numbers[i]), "%u", i);
		overscan_values[i + 1].value = overscan_values[i + 1].label = overscan_numbers[i];
	}
	overscan_values[MAX_OVERSCAN + 2].value = NULL;
	overscan_values[MAX_OVERSCAN + 2].label = NULL;
}

//A frontend that predates categorised options gets the same list flattened into
//the "description; value|value" strings the first interface used.
static void publish_core_options_v0(retro_environment_t re)
{
	static struct retro_variable vars[NUM_CORE_OPTIONS + 1];
	static char descs[NUM_CORE_OPTIONS][512];
	for (size_t i = 0; i < NUM_CORE_OPTIONS; i++)
	{
		const core_option *opt = core_options + i;
		int len = snprintf(descs[i], sizeof(descs[i]), "%s; ", opt->desc);
		for (size_t j = 0; opt->values[j].value && len < (int)sizeof(descs[i]); j++)
		{
			len += snprintf(descs[i] + len, sizeof(descs[i]) - len, j ? "|%s" : "%s", opt->values[j].value);
		}
		vars[i].key = opt->key;
		vars[i].value = descs[i];
	}
	vars[NUM_CORE_OPTIONS].key = NULL;
	vars[NUM_CORE_OPTIONS].value = NULL;
	re(RETRO_ENVIRONMENT_SET_VARIABLES, (void *)vars);
}

static void publish_core_options(retro_environment_t re)
{
	//A frontend may set the environment callback more than once, so this has to
	//survive being called again; the lists it builds are the same every time.
	build_dynamic_values();

	unsigned version = 0;
	if (!re(RETRO_ENVIRONMENT_GET_CORE_OPTIONS_VERSION, &version) || version < 2) {
		publish_core_options_v0(re);
		return;
	}

	static struct retro_core_option_v2_category categories[] = {
		{ "system", "System", "Which machine is emulated, how it is clocked and where it thinks it is." },
		{ "video",  "Video",  "How much of the picture the core hands to the frontend." },
		{ "audio",  "Audio",  "Mixing and filtering of the emulated sound chips." },
		{ "input",  "Input",  "What is plugged into the console's controller ports." },
		{ NULL, NULL, NULL }
	};
	static struct retro_core_option_v2_definition definitions[NUM_CORE_OPTIONS + 1];
	for (size_t i = 0; i < NUM_CORE_OPTIONS; i++)
	{
		const core_option *opt = core_options + i;
		definitions[i].key = (char *)opt->key;
		definitions[i].desc = (char *)opt->desc;
		definitions[i].desc_categorized = NULL;
		definitions[i].info = (char *)opt->info;
		definitions[i].info_categorized = NULL;
		definitions[i].category_key = (char *)opt->category;
		size_t j;
		for (j = 0; opt->values[j].value && j < RETRO_NUM_CORE_OPTION_VALUES_MAX - 1; j++)
		{
			definitions[i].values[j] = opt->values[j];
		}
		definitions[i].values[j].value = NULL;
		definitions[i].values[j].label = NULL;
		definitions[i].default_value = (char *)opt->values[0].value;
	}
	memset(definitions + NUM_CORE_OPTIONS, 0, sizeof(definitions[0]));
	static struct retro_core_options_v2 options = { categories, definitions };
	re(RETRO_ENVIRONMENT_SET_CORE_OPTIONS_V2, &options);
}

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

	publish_core_options(re);

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
		//Same story for the TMSS ROM every model from the Mega Drive 2 on needs.
		//Genesis Plus GX reads the same ROM as bios_MD.bin, which is the name it
		//is most likely to have in a RetroArch system directory already.
		char *tmss = alloc_concat(system_dir, "/tmss.md");
		char *gpgx_tmss = alloc_concat(system_dir, "/bios_MD.bin");
		FILE *f;
		if (!(f = fopen(tmss, "rb")) && (f = fopen(gpgx_tmss, "rb"))) {
			free(tmss);
			tmss = gpgx_tmss;
		} else {
			free(gpgx_tmss);
		}
		if (f) {
			fclose(f);
		}
		config = tern_insert_path(config, "system\0tmss_path\0", (tern_val){.ptrval = tmss}, TVAL_PTR);
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

//mix_and_convert() mixes into a buffer sized from what render_audio_initialized()
//was told, so a batch may be no larger than that.
#define AUDIO_BATCH_FRAMES 512

//The mix sits on a DC offset - a few hundred at silence, how much depends on
//what the YM2612 and the PSG are left at - which the console's output
//capacitors keep off the line. Handed to the frontend as it was, every moment
//the frontend's audio stalls (the first seconds under a heavy shader, a
//refresh rate switch with Sync to Content Refresh) dropped the output to zero
//and brought it back to the offset: a pop each time (issue #17). A first
//order high-pass at 5 Hz takes the offset off and leaves everything audible
//alone. It starts from the first sample it sees, so starting the audio is not
//a step either.
#define DC_BLOCK_HZ 5.0
static int32_t sample_rate;
static float dc_last_in[2], dc_last_out[2];
static uint8_t dc_primed;

static void dc_block(int16_t *samples, uint32_t frames)
{
	const float r = 1.0f - (float)(2.0 * 3.14159265358979 * DC_BLOCK_HZ) / (float)sample_rate;
	if (!dc_primed) {
		dc_last_in[0] = samples[0];
		dc_last_in[1] = samples[1];
		dc_last_out[0] = dc_last_out[1] = 0.0f;
		dc_primed = 1;
	}
	for (uint32_t i = 0; i < frames * 2; i++)
	{
		const int ch = i & 1;
		const float in = samples[i];
		const float out = in - dc_last_in[ch] + r * dc_last_out[ch];
		dc_last_in[ch] = in;
		dc_last_out[ch] = out;
		samples[i] = out > 32767.0f ? 32767 : out < -32768.0f ? -32768 : (int16_t)out;
	}
}

//Hands the frontend everything every source has ready. Asking for no more than
//that is what keeps a source from being mixed past what it has written, which
//would be heard as a gap.
static void flush_audio(void)
{
	if (!retro_audio_sample_batch) {
		return;
	}
	for (uint32_t remaining = audio_buffered(); remaining; )
	{
		uint32_t frames = remaining > AUDIO_BATCH_FRAMES ? AUDIO_BATCH_FRAMES : remaining;
		int16_t buffer[AUDIO_BATCH_FRAMES * 2];
		mix_and_convert((uint8_t *)buffer, frames * 2 * sizeof(int16_t), NULL);
		dc_block(buffer, frames);
		retro_audio_sample_batch(buffer, frames);
		remaining -= frames;
	}
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

//Warnings and errors - a missing BIOS or TMSS ROM among them - otherwise went
//to stderr only: not into the frontend's log, and not in front of the user,
//for whom a load that failed for a missing file looked like the core doing
//nothing at all (issue #69). The last one is kept for retro_load_game() to put
//on screen if the load fails.
static retro_log_printf_t retro_log;
static char last_warning[256];
static void lib_log_handler(log_level level, char *message)
{
	if (level < WARN) {
		return;
	}
	snprintf(last_warning, sizeof(last_warning), "%s", message);
	size_t len = strlen(last_warning);
	while (len && (last_warning[len - 1] == '\n' || last_warning[len - 1] == '\r')) {
		last_warning[--len] = 0;
	}
	if (retro_log) {
		retro_log(level > WARN ? RETRO_LOG_ERROR : RETRO_LOG_WARN, "%s\n", last_warning);
	}
}

RETRO_API void retro_init(void)
{
	struct retro_log_callback log_cb;
	retro_log = retro_environment(RETRO_ENVIRONMENT_GET_LOG_INTERFACE, &log_cb) ? log_cb.log : NULL;
	register_log_handler(lib_log_handler);
	render_audio_initialized(RENDER_AUDIO_S16, 53693175 / (7 * 6 * 4), 2, AUDIO_BATCH_FRAMES, sizeof(int16_t));
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

//The frontend's own value for an option, or NULL if it has none.
static const char *core_option_value(const char *key)
{
	struct retro_variable var = { .key = key };
	if (!retro_environment(RETRO_ENVIRONMENT_GET_VARIABLE, &var)) {
		return NULL;
	}
	return var.value;
}

//Every option that names a config path is simply written into the tree the
//emulator reads while it builds a machine, which is why this has to run before
//alloc_config_system() rather than at any later point.
static void apply_core_options(void)
{
	for (size_t i = 0; i < NUM_CORE_OPTIONS; i++)
	{
		const core_option *opt = core_options + i;
		if (!opt->config_path) {
			continue;
		}
		const char *value = core_option_value(opt->key);
		if (!value) {
			//A frontend with no options support at all answers nothing, so the
			//documented default goes in rather than leaving the emulator to fall
			//back on whatever its own default happens to be.
			value = opt->values[0].value;
		}
		//The tree owns what it is given and the old value is not reachable from
		//anywhere else once it has been replaced.
		config = tern_insert_path(config, (char *)opt->config_path, (tern_val){.ptrval = strdup(value)}, TVAL_PTR);
	}
}

static vid_std video_standard;
static uint32_t last_width, last_height;
static uint8_t frame_presented;
static void present_framebuffer(uint8_t which, int width);
static uint32_t overscan_top, overscan_bot, overscan_left, overscan_right;
static void override_overscan(const char *key, uint32_t *dst)
{
	const char *value = core_option_value(key);
	if (value && strcmp(value, "auto")) {
		*dst = atoi(value);
	}
}

//What "Auto" crops: the borders a TV of the region would have hidden anyway.
//RETRO_ENVIRONMENT_GET_OVERSCAN, which used to decide this, is deprecated and a
//frontend may stop answering it at all, so the core no longer asks - the option
//alone says what to crop, and 0 turns cropping off.
static void update_overscan(void)
{
	if (video_standard == VID_NTSC) {
		overscan_top = 11;
		overscan_bot = 8;
	} else {
		overscan_top = 30;
		overscan_bot = 24;
	}
	overscan_left = 13;
	overscan_right = 14;

	//Unlike the rest, cropping is this file's own doing rather than something the
	//emulator reads out of the config tree, so it can follow the option as soon
	//as it changes.
	override_overscan("blastem_overscan_top", &overscan_top);
	override_overscan("blastem_overscan_bottom", &overscan_bot);
	override_overscan("blastem_overscan_left", &overscan_left);
	override_overscan("blastem_overscan_right", &overscan_right);
}

//The shape of the picture on a TV. A frame's pixels are not square: the VDP
//puts them out at its dot clock, the master clock over 8 in H40 and over 10 in
//H32, while a square pixel is 135/11 MHz on a 525-line system and 14.75 MHz on
//a 625-line one - both for 480 and 576 lines, and a 240p line is two of those
//tall. That gives the familiar 32:35 (H40) and 8:7 (H32) for NTSC. Reporting 0
//left the frontend to assume square pixels (issue #37), and the ratio that
//replaced it on the first frame ignored the crop and the H32 mode.
static float frame_aspect(unsigned width, unsigned height, uint8_t h40)
{
	double square_rate = video_standard == VID_NTSC ? 135.0 / 11.0 : 14.75;
	double master_clock = video_standard == VID_NTSC ? 53.693175 : 53.203395;
	double pixel_aspect = square_rate / (master_clock / (h40 ? 8 : 10)) / 2.0;
	return (float)(width * pixel_aspect / height);
}

static struct retro_system_av_info av_info;
//Worked out when the machine is built rather than when the frontend asks for it:
//run-ahead's second instance is created with retro_init(), retro_load_game() and
//the callbacks alone, and is never asked for AV info at all. An instance that
//waited to be asked would keep overscan_* at zero, present the uncropped 347x243
//picture the cropped one is meant to replace, and never hand render_audio a
//sample rate - and in that mode it is the second instance's frames that reach
//the screen.
static void update_av_info(void)
{
	update_overscan();
	last_width = LINEBUF_SIZE;
	av_info.geometry.base_width = av_info.geometry.max_width = LINEBUF_SIZE - (overscan_left + overscan_right);
	av_info.geometry.base_height = (video_standard == VID_NTSC ? 243 : 294) - (overscan_top + overscan_bot);
	last_height = av_info.geometry.base_height;
	av_info.geometry.max_height = av_info.geometry.base_height * 2;
	//Assumes H40 until the first frame says otherwise, as most games are.
	av_info.geometry.aspect_ratio = frame_aspect(av_info.geometry.base_width, av_info.geometry.base_height, 1);
	double master_clock = video_standard == VID_NTSC ? 53693175 : 53203395;
	double lines = video_standard == VID_NTSC ? 262 : 313;
	av_info.timing.fps = master_clock / (3420.0 * lines);
	av_info.timing.sample_rate = master_clock / (7 * 6 * 24); //sample rate of YM2612
	sample_rate = av_info.timing.sample_rate;
	render_audio_initialized(RENDER_AUDIO_S16, sample_rate, 2, AUDIO_BATCH_FRAMES, sizeof(int16_t));
	//force adjustment of resampling parameters since target sample rate may have changed slightly
	current_system->set_speed_percent(current_system, 100);
}

RETRO_API void retro_get_system_av_info(struct retro_system_av_info *info)
{
	*info = av_info;
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
static void poll_input(void);
RETRO_API void retro_run(void)
{
	bool options_updated = false;
	if (retro_environment(RETRO_ENVIRONMENT_GET_VARIABLE_UPDATE, &options_updated) && options_updated) {
		apply_core_options();
		//Gains, the DAC and what is in the controller ports are re-read from the
		//config tree on demand, the same way the standalone picks up a change in
		//its settings menu. Everything else - the model, the region, the clock
		//divider - is read while a machine is built and so waits for the next
		//load.
		//update_av_info() re-runs render_audio_initialized() with the batch size
		//below, so the re-init this used to do by hand is already covered.
		update_av_info();
		if (current_system->config_updated) {
			current_system->config_updated(current_system);
		}
	}
	//once per frame, before the machine that will read it runs
	poll_input();
	frame_presented = 0;
	if (started) {
		current_system->resume_context(current_system);
	} else {
		current_system->start_context(current_system, NULL);
		started = 1;
	}
	//A system that returned without presenting anything still owes the frontend
	//a frame, so that it gets one per call and its pacing and audio sync have
	//something to run against. The media player never has video at all, and a
	//Sega CD does not reach the end of a frame on the call that starts it.
	if (!frame_presented) {
		present_framebuffer(render_get_active_framebuffer(), LINEBUF_SIZE);
	}
	flush_audio();
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
	//the length itself goes in front of the state, so it has to fit as well
	if (*buffer + sizeof(size_t) > size) {
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
static bool load_game(const struct retro_game_info *game);
RETRO_API bool retro_load_game(const struct retro_game_info *game)
{
	last_warning[0] = 0;
	if (load_game(game)) {
		return 1;
	}
	//The frontend only says the content failed to load, or not even that; say why.
	if (last_warning[0]) {
		struct retro_message msg = { last_warning, 600 };
		retro_environment(RETRO_ENVIRONMENT_SET_MESSAGE, &msg);
	}
	return 0;
}

//A MegaWiFi cartridge's module keeps its settings and flash next to the game's
//other saves, the frontend's save directory if it has one.
static void set_megawifi_storage(void)
{
	const char *save_dir = NULL;
	if (!retro_environment(RETRO_ENVIRONMENT_GET_SAVE_DIRECTORY, &save_dir) || !save_dir || !*save_dir) {
		save_dir = media.dir;
	}
	if (!save_dir || !media.name) {
		megawifi_set_storage_prefix(NULL);
		return;
	}
	char *prefix = path_append(save_dir, media.name);
	megawifi_set_storage_prefix(prefix);
	free(prefix);
}

static bool load_game(const struct retro_game_info *game)
{
	serialize_size_cache = 0;
	stype = SYSTEM_UNKNOWN;
	if (setjmp(fatal_recover)) {
		release_media();
		current_system = NULL;
		return 0;
	}
	fatal_recover_valid = 1;
	apply_core_options();
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
	set_megawifi_storage();
	current_system = alloc_config_system(stype, &media, 0, 0);
	fatal_recover_valid = 0;
	if (!current_system) {
		release_media();
		return 0;
	}

	unsigned format = RETRO_PIXEL_FORMAT_XRGB8888;
	retro_environment(RETRO_ENVIRONMENT_SET_PIXEL_FORMAT, &format);

	update_av_info();

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
	dc_primed = 0;
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

//Hands the frontend a frame. Separate from render_framebuffer_updated() because
//the emulator asking to be let go is what ends a frame, and a frame this file
//presents on its own behalf must not ask for that: the request would still be
//standing on the next retro_run() and skip the emulation loop entirely.
static void present_framebuffer(uint8_t which, int width)
{
	unsigned height = (video_standard == VID_NTSC ? 243 : 294) - (overscan_top + overscan_bot);
	uint8_t h40 = width == LINEBUF_SIZE;
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
			.aspect_ratio = frame_aspect(width, base_height, h40)
		};
		retro_environment(RETRO_ENVIRONMENT_SET_GEOMETRY, &geometry);
		last_width = width;
		last_height = height;
	}
	retro_video_refresh(fb + overscan_left + LINEBUF_SIZE * overscan_top, width, height, LINEBUF_SIZE * sizeof(uint32_t));
	frame_presented = 1;
}

void render_framebuffer_updated(uint8_t which, int width)
{
	present_framebuffer(which, width);
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

//Sampled once per frame from retro_run(). The standalone instead polls part way
//through a frame, from io_data_read(), whenever the game reads a pad and
//io_port::last_poll_cycle says the previous poll is old enough. That cannot work
//here: the frontend hands the core one input snapshot per retro_run() and
//expects the frame it gets back to depend on nothing but that snapshot and the
//machine state, because run-ahead, preemptive frames, rewind and netplay all
//replay frames from a save state. last_poll_cycle is not in a save state, so
//after a rollback it still holds a cycle count from the frame that was thrown
//away and the game's own pad read gets skipped as "too recent". Run-ahead lands
//squarely on that case - the read in the replayed frame falls on the very cycle
//the discarded frame polled at - so the core would only ever poll during the
//frames the frontend feeds it stale input with, and the player's presses would
//never arrive.
static void poll_input(void)
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

void process_events()
{
	//input is sampled in retro_run(), see poll_input()
}

uint8_t render_is_audio_sync(void)
{
	//A frame ends when the VDP hands over a framebuffer, so audio is not what
	//paces emulation here and each source can keep its own ring buffer. That is
	//the only arrangement that survives sources running at rates of their own:
	//the Sega CD's CD-DA and PCM chip fill at their own pace, and in the sync
	//path a source that filled twice before the others filled once had a whole
	//buffer dropped on the floor.
	return 0;
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
	//Sizes each source's ring: render_audio_source() rounds min_buffered * 4 *
	//channels up to a power of two. This leaves room for around a tenth of a
	//second, so a source that runs ahead of the others has somewhere to put it.
	return 2048;
}

uint32_t render_audio_syncs_per_sec(void)
{
	//How often a source publishes what it has written. Often enough that a
	//frame's worth arrives in pieces rather than all at once.
	return 1000;
}

void render_audio_created(audio_source *src)
{
}

void render_do_audio_ready(audio_source *src)
{
	//Publish what has been written; the mixing happens once the frame is done
	src->read_end = src->buffer_pos;
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
extern const char systems_cfg_data[];
char *read_bundled_file(char *name, uint32_t *sizeret)
{
	//A libretro core is a single file with nothing alongside it, so the two data
	//files the emulator cannot work without are compiled into it.
	const char *data = NULL;
	if (!strcmp(name, "rom.db")) {
		data = rom_db_data;
	} else if (!strcmp(name, "systems.cfg")) {
		data = systems_cfg_data;
	}
	if (!data) {
		return NULL;
	}
	*sizeret = strlen(data);
	char *ret = malloc(*sizeret+1);
	memcpy(ret, data, *sizeret + 1);
	return ret;
}
