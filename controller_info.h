#ifndef CONTROLLER_INFO_H_
#define CONTROLLER_INFO_H_
#include <stdint.h>

enum {
	TYPE_UNKNOWN,
	TYPE_GENERIC_MAPPING,
	TYPE_XBOX,
	TYPE_PSX,
	TYPE_NINTENDO,
	TYPE_SEGA
};

enum {
	SUBTYPE_UNKNOWN,
	SUBTYPE_XBOX,
	SUBTYPE_X360,
	SUBTYPE_XBONE,
	SUBTYPE_PS2,
	SUBTYPE_PS3,
	SUBTYPE_PS4,
	SUBTYPE_WIIU,
	SUBTYPE_SWITCH,
	SUBTYPE_GENESIS,
	SUBTYPE_SATURN,
	SUBTYPE_NUM
};

enum {
	VARIANT_NORMAL,
	VARIANT_6B_BUMPERS, //C and Z positions are RB and LB respectively
	VARIANT_6B_RIGHT, //C and Z positions are RT and RB respectively
	VARIANT_3BUTTON, //3-button Gen/MD controller
	VARIANT_8BUTTON, //Modern 8-button Gen/MD style controller (retro-bit, 8bitdo M30, etc.)
	VARIANT_NUM
};

typedef struct {
	char const *name;
	uint8_t    type;
	uint8_t    subtype;
	uint8_t    variant;
} controller_info;

controller_info get_controller_info(int index);
const char *get_button_label(controller_info *info, int button);
const char *get_axis_label(controller_info *info, int axis);
void save_controller_info(int joystick, controller_info *info);
void save_controller_mapping(int joystick, char *mapping_string);
void delete_controller_info(void);
void controller_add_mappings(void);
char *make_controller_type_key(controller_info *info);
char *make_human_readable_type_name(controller_info *info);

#endif //CONTROLLER_INFO_H_