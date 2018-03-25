#ifndef SFNT_H_
#define SFNT_H_

#include <stdint.h>
enum {
	CONTAINER_TTF,
	CONTAINER_TTC,
	CONTAINER_DFONT
};

enum {
	SFNT_COPYRIGHT,
	SFNT_FAMILY,
	SFNT_SUBFAMILY,
	SFNT_SUBFAMILY_UNIQUE,
	SFNT_FULL_NAME,
	SFNT_VERSION,
	SFNT_POSTSCRIPT,
	//TODO: add the rest of the name IDs
};

typedef struct sfnt_container sfnt_container;
typedef struct {
	uint8_t        *data;
	sfnt_container *container;
	uint32_t       offset;
	uint16_t       num_entries;
} sfnt_table;

struct sfnt_container {
	uint8_t    *blob;
	sfnt_table *tables;
	uint32_t   size;
	uint8_t    num_fonts;
	uint8_t    container_type;
};

sfnt_container *load_sfnt(uint8_t *buffer, uint32_t size);
char *sfnt_name(sfnt_table *sfnt, uint16_t name_type);
uint8_t *sfnt_flatten(sfnt_table *sfnt, uint32_t *size_out);
sfnt_table *sfnt_subfamily_by_names(sfnt_container *sfnt, const char **names);
void sfnt_free(sfnt_container *sfnt);

#endif // SFNT_H_