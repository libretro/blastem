#include <string.h>
#include <stdlib.h>
#ifndef USE_FBDEV
#include "render_sdl.h"
#endif
#include "controller_info.h"
#include "config.h"
#include "util.h"
#include "blastem.h"
#include "bindings.h"

typedef struct {
	char const      *name;
	controller_info info;
} heuristic;

static heuristic heuristics[] = {
	//TODO: Add more heuristic rules
	{"DualShock 4", {.type = TYPE_PSX, .subtype = SUBTYPE_PS4}},
	{"PS4", {.type = TYPE_PSX, .subtype = SUBTYPE_PS4}},
	{"PS3", {.type = TYPE_PSX, .subtype = SUBTYPE_PS3}},
	{"X360", {.type = TYPE_XBOX, .subtype = SUBTYPE_X360}},
	{"Xbox 360", {.type = TYPE_XBOX, .subtype = SUBTYPE_X360}},
	{"X-box 360", {.type = TYPE_XBOX, .subtype = SUBTYPE_X360}},
	{"Xbox One", {.type = TYPE_XBOX, .subtype = SUBTYPE_XBONE}},
	{"X-box One", {.type = TYPE_XBOX, .subtype = SUBTYPE_XBONE}},
	{"WiiU", {.type = TYPE_NINTENDO, .subtype = SUBTYPE_WIIU}},
	{"Wii U", {.type = TYPE_NINTENDO, .subtype = SUBTYPE_WIIU}},
	{"Nintendo Switch", {.type = TYPE_NINTENDO, .subtype = SUBTYPE_SWITCH}},
	{"Saturn", {.type = TYPE_SEGA, .subtype = SUBTYPE_SATURN}}
};
const uint32_t num_heuristics = sizeof(heuristics)/sizeof(*heuristics);

static tern_node *info_config;
static uint8_t loaded;
static const char *subtype_names[] = {
	"unknown",
	"xbox",
	"xbox 360",
	"xbone",
	"ps2",
	"ps3",
	"ps4",
	"wiiu",
	"switch",
	"genesis",
	"saturn"
};
static const char *subtype_human_names[] = {
	"unknown",
	"Xbos",
	"Xbox 360",
	"Xbox One",
	"PS2",
	"PS3",
	"PS4",
	"Wii-U",
	"Switch",
	"Genesis",
	"Saturn"
};
static const char *variant_names[] = {
	"normal",
	"6b bumpers",
	"6b right",
	"3button",
	"6button",
	"8button"
};

static void load_ctype_config(void)
{
	if (!loaded) {
		info_config = load_overrideable_config("controller_types.cfg", "controller_types.cfg", NULL);
		loaded = 1;
	}
}

controller_info get_controller_info(int joystick)
{
#ifndef USE_FBDEV
	load_ctype_config();
	char guid_string[33];
	SDL_Joystick *stick = render_get_joystick(joystick);
	SDL_GameController *control = render_get_controller(joystick);
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(stick), guid_string, sizeof(guid_string));
	tern_node *info = tern_find_node(info_config, guid_string);
	if (info) {
		controller_info res;
		char *subtype = tern_find_ptr(info, "subtype");
		res.subtype = SUBTYPE_UNKNOWN;
		if (subtype) {
			for (int i = 0; i < SUBTYPE_NUM; i++)
			{
				if (!strcmp(subtype_names[i], subtype)) {
					res.subtype = i;
					break;
				}
			}
		}
		switch (res.subtype)
		{
		case SUBTYPE_XBOX:
		case SUBTYPE_X360:
		case SUBTYPE_XBONE:
			res.type = TYPE_XBOX;
			break;
		case SUBTYPE_PS2:
		case SUBTYPE_PS3:
		case SUBTYPE_PS4:
			res.type = TYPE_PSX;
			break;
		case SUBTYPE_WIIU:
		case SUBTYPE_SWITCH:
			res.type = TYPE_NINTENDO;
			break;
		case SUBTYPE_GENESIS:
		case SUBTYPE_SATURN:
			res.type = TYPE_SEGA;
			break;
		default:
			res.type = TYPE_UNKNOWN;
			break;
		}
		char *variant = tern_find_ptr(info, "variant");
		res.variant = VARIANT_NORMAL;
		if (variant) {
			for (int i = 0; i < VARIANT_NUM; i++)
			{
				if (!strcmp(variant_names[i], variant)) {
					res.variant = i;
					break;
				}
			}
		}
		res.name = control ? SDL_GameControllerName(control) : SDL_JoystickName(stick);
		if (control) {
			SDL_GameControllerClose(control);
		}
		return res;
	}
	if (!control) {
		return (controller_info) {
			.type = TYPE_UNKNOWN,
			.subtype = SUBTYPE_UNKNOWN,
			.variant = VARIANT_NORMAL,
			.name = SDL_JoystickName(stick)
		};
	}
	const char *name = SDL_GameControllerName(control);
	SDL_GameControllerClose(control);
	for (uint32_t i = 0; i < num_heuristics; i++)
	{
		if (strstr(name, heuristics[i].name)) {
			controller_info res = heuristics[i].info;
			res.name = name;
			return res;
		}
	}
#else
	const char *name = "Unknown";
#endif
	//default to a 360
	return (controller_info){
		.type = TYPE_GENERIC_MAPPING,
		.subtype = SUBTYPE_UNKNOWN,
		.variant = VARIANT_NORMAL,
		.name = name
	};
}

static void mappings_iter(char *key, tern_val val, uint8_t valtype, void *data)
{
#ifndef USE_FBDEV
	if (valtype != TVAL_NODE) {
		return;
	}
	char *mapping = tern_find_ptr(val.ptrval, "mapping");
	if (mapping) {
		const char *parts[] = {key, ",", mapping};
		char * full = alloc_concat_m(3, parts);
		SDL_GameControllerAddMapping(full);
		free(full);
	}
#endif
}

void controller_add_mappings(void)
{
	load_ctype_config();
	if (info_config) {
		tern_foreach(info_config, mappings_iter, NULL);
	}
}

void save_controller_info(int joystick, controller_info *info)
{
#ifndef USE_FBDEV
	char guid_string[33];
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(render_get_joystick(joystick)), guid_string, sizeof(guid_string));
	tern_node *existing = tern_find_node(info_config, guid_string);
	existing = tern_insert_ptr(existing, "subtype", strdup(subtype_names[info->subtype]));
	existing = tern_insert_ptr(existing, "variant", strdup(variant_names[info->variant]));
	info_config = tern_insert_node(info_config, guid_string, existing);
	persist_config_at(config, info_config, "controller_types.cfg");
	handle_joy_added(joystick);
#endif	
}

void save_controller_mapping(int joystick, char *mapping_string)
{
#ifndef USE_FBDEV
	char guid_string[33];
	SDL_JoystickGetGUIDString(SDL_JoystickGetGUID(render_get_joystick(joystick)), guid_string, sizeof(guid_string));
	tern_node *existing = tern_find_node(info_config, guid_string);
	existing = tern_insert_ptr(existing, "mapping", mapping_string);
	info_config = tern_insert_node(info_config, guid_string, existing);
	persist_config_at(config, info_config, "controller_types.cfg");
	const char *parts[] = {guid_string, ",", mapping_string};
	char * full = alloc_concat_m(3, parts);
	SDL_GameControllerAddMapping(full);
	free(full);
	handle_joy_added(joystick);
#endif
}

void delete_controller_info(void)
{
	delete_custom_config_at("controller_types.cfg");
	loaded = 0;
	tern_free(info_config);
	info_config = NULL;
	render_reset_mappings();
}

char const *labels_xbox[] = {
	"A", "B", "X", "Y", "Back", NULL, "Start", "Click", "Click", "White", "Black", "LT", "RT"
};
char const *labels_360[] = {
	"A", "B", "X", "Y", "Back", "Xbox", "Start", "Click", "Click", "LB", "RB", "LT", "RT"
};
static char const *labels_xbone[] = {
	"A", "B", "X", "Y", "View", "Xbox", "Menu", "Click", "Click", "LB", "RB", "LT", "RT"
};
static char const *labels_ps3[] = {
	"cross", "circle", "square", "triangle", "Select", "PS", "Start", "L3", "R3", "L1", "R1", "L2", "R2"
};
static char const *labels_ps4[] = {
	"cross", "circle", "square", "triangle", "Share", "PS", "Options", "L3", "R3", "L1", "R1", "L2", "R2"
};
static char const *labels_nintendo[] = {
	"B", "A", "Y", "X", "-", "Home", "+", "Click", "Click", "L", "R", "ZL", "ZR"
};
static char const *labels_genesis[] = {
	"A", "B", "X", "Y", NULL, NULL, "Start", NULL, NULL, "Z", "C", NULL, "Mode"
};
static char const *labels_genesis_3button[] = {
	"A", "B", NULL, NULL, NULL, NULL, "Start", NULL, NULL, NULL, "C", NULL, "Mode"
};
static char const *labels_genesis_8button[] = {
	"A", "B", "X", "Y", "Mode", NULL, "Start", NULL, NULL, "Z", "C", "L", "R"
};
static char const *labels_saturn[] = {
	"A", "B", "X", "Y", NULL, NULL, "Start", NULL, NULL, "Z", "C", "LT", "RT"
};

static const char** label_source(controller_info *info)
{
	if (info->type == TYPE_UNKNOWN || info->type == TYPE_GENERIC_MAPPING || info->subtype ==SUBTYPE_X360) {
		return labels_360;
	} else if (info->type == TYPE_NINTENDO) {
		return labels_nintendo;
	} else if (info->type == TYPE_PSX) {
		if (info->subtype == SUBTYPE_PS4) {
			return labels_ps4;
		} else {
			return labels_ps3;
		}
	} else if (info->type == TYPE_XBOX) {
		if (info->subtype == SUBTYPE_XBONE) {
			return labels_xbone;
		} else {
			return labels_xbox;
		}
	} else {
		if (info->subtype == SUBTYPE_GENESIS) {
			if (info->variant == VARIANT_8BUTTON) {
				return labels_genesis_8button;
			} else if (info->variant == VARIANT_3BUTTON) {
				return labels_genesis_3button;
			} else {
				return labels_genesis;
			}
		} else {
			return labels_saturn;
		}
	}
}

const char *get_button_label(controller_info *info, int button)
{
#ifndef USE_FBDEV
	if (button >= SDL_CONTROLLER_BUTTON_DPAD_UP) {
		static char const * dirs[] = {"Up", "Down", "Left", "Right"};
		return dirs[button - SDL_CONTROLLER_BUTTON_DPAD_UP];
	}
#endif
	return label_source(info)[button];
}

static char const *axis_labels[] = {
	"Left X", "Left Y", "Right X", "Right Y"
};
const char *get_axis_label(controller_info *info, int axis)
{
#ifndef USE_FBDEV
	if (axis < SDL_CONTROLLER_AXIS_TRIGGERLEFT) {
		return axis_labels[axis];
	} else {
		return label_source(info)[axis - SDL_CONTROLLER_AXIS_TRIGGERLEFT + SDL_CONTROLLER_BUTTON_RIGHTSHOULDER + 1];
	}
#else
	return NULL;
#endif
}

char *make_controller_type_key(controller_info *info)
{
	const char *subtype;
	if (info->subtype == SUBTYPE_UNKNOWN) {
		switch(info->type)
		{
		case TYPE_XBOX:
			subtype = subtype_names[SUBTYPE_X360];
			break;
		case TYPE_PSX:
			subtype = subtype_names[SUBTYPE_PS4];
			break;
		case TYPE_NINTENDO:
			subtype = subtype_names[SUBTYPE_SWITCH];
			break;
		default:
			subtype = "unknown";
		}
	} else {
		subtype = subtype_names[info->subtype];
	}
	const char *variant = variant_names[info->variant];
	const char *parts[] = {subtype, "_", variant};
	char *ret = alloc_concat_m(3, parts);
	for (char *cur = ret; *cur; cur++)
	{
		if (*cur == ' ')
		{
			*cur = '_';
		}
	}
	return ret;
}

char *make_human_readable_type_name(controller_info *info)
{
	const char *base = subtype_human_names[info->subtype];
	char *prefix;
	if (info->variant == VARIANT_NORMAL) {
		prefix = "Normal ";
	} else {
		static const char *parts[] = {"6 button (", NULL, "/", NULL, ") "};
#ifdef USE_FBDEV
		parts[1] = parts[3] = "??";
#else
		if (info->variant == VARIANT_6B_BUMPERS) {
			parts[1] = get_button_label(info, SDL_CONTROLLER_BUTTON_LEFTSHOULDER);
			parts[3] = get_button_label(info, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
		} else {
			parts[1] = get_button_label(info, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER);
			parts[3] = get_axis_label(info, SDL_CONTROLLER_AXIS_TRIGGERRIGHT);
		}
#endif
		prefix = alloc_concat_m(5, parts);
	}
	char *ret = alloc_concat(prefix, base);
	if (info->variant != VARIANT_NORMAL) {
		free(prefix);
	}
	return ret;
}

