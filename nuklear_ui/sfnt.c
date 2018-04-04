#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include "sfnt.h"
#include "../util.h"

static uint32_t big32(uint8_t *src)
{
	uint32_t ret = *(src++) << 24;
	ret |= *(src++) << 16;
	ret |= *(src++) << 8;
	ret |= *src;
	return ret;
}

static uint32_t big16(uint8_t *src)
{
	uint32_t ret = *(src++) << 8;
	ret |= *src;
	return ret;
}

#define MIN_RESOURCE_MAP_SIZE (16 + 12 + 2 + 8)

sfnt_container *load_sfnt(uint8_t *buffer, uint32_t size)
{
	if (size < 0x100) {
		return NULL;
	}
	uint32_t sfnt_res_count, sfnt_res_offset, res_offset;
	uint8_t type;
	if (!memcmp(buffer, "true", 4) || !memcmp(buffer, "OTTO", 4) || !memcmp(buffer, "typ1", 4) || !memcmp(buffer, "\0\x01\0\0", 4)) {
		type = CONTAINER_TTF;
	} else if (!memcmp(buffer, "ttcf", 4)) {
		type = CONTAINER_TTC;
	} else {
		static uint8_t all_zeroes[16];
		uint32_t resource_map_off = big32(buffer + 4);
		if (resource_map_off + MIN_RESOURCE_MAP_SIZE > size) {
			return NULL;
		}
		//first 16 bytes of map should match header or be all zeroes
		if (memcmp(buffer, buffer + resource_map_off, 16) && memcmp(all_zeroes, buffer + resource_map_off, 16)) {
			return NULL;
		}
		uint32_t type_start_off = resource_map_off + big16(buffer + resource_map_off + 24);
		if (type_start_off + sizeof(uint16_t) > size) {
			return NULL;
		}
		uint32_t num_types = 1 + big16(buffer + type_start_off);
		if (type_start_off + sizeof(uint16_t) + 8 * num_types > size) {
			return NULL;
		}
		res_offset = big32(buffer);
		if (res_offset > size) {
			return NULL;
		}
		uint8_t *cur = buffer + type_start_off + 2;
		sfnt_res_count = 0;
		for (uint32_t i = 0; i < num_types; i++, cur += 8)
		{
			if (!memcmp("sfnt", cur, 4)) {
				sfnt_res_count = 1 + big16(cur + 4);
				sfnt_res_offset = type_start_off + big16(cur + 6);
				if (sfnt_res_offset + sfnt_res_count * 12 > size) {
					return NULL;
				}
				type = CONTAINER_DFONT;
				break;
			}
		}
		if (!sfnt_res_count) {
			//No "sfnt" resources in this dfont
			return NULL;
		}
	}
	sfnt_container *sfnt = calloc(1, sizeof(sfnt_container));
	sfnt->blob = buffer;
	sfnt->size = size;
	sfnt->container_type = type;
	switch (type)
	{
	case CONTAINER_TTF:
		sfnt->num_fonts = 1;
		sfnt->tables = calloc(1, sizeof(sfnt_table));
		sfnt->tables->container = sfnt;
		sfnt->tables->data = buffer + 0xC;
		sfnt->tables->num_entries = big16(buffer + 4);
		sfnt->tables->offset = 0;
		break;
	case CONTAINER_TTC: {
		sfnt->num_fonts = big32(buffer+8);
		sfnt->tables = calloc(sfnt->num_fonts, sizeof(sfnt_table));
		uint8_t *offsets = buffer + 0xC;
		for (int i = 0; i < sfnt->num_fonts; i++, offsets += sizeof(uint32_t))
		{
			uint32_t offset = big32(offsets);
			sfnt->tables[i].data = buffer + offset + 0xC;
			sfnt->tables[i].container = sfnt;
			sfnt->tables[i].num_entries = big16(buffer + offset + 4);
			sfnt->tables[i].offset = 0;
		}
		break;
	}
	case CONTAINER_DFONT:{
		sfnt->num_fonts = sfnt_res_count;
		sfnt->tables = calloc(sfnt->num_fonts, sizeof(sfnt_table));
		uint8_t *cur = buffer + sfnt_res_offset;
		for (int i = 0; i < sfnt->num_fonts; i++, cur += 12)
		{
			uint32_t offset = res_offset + (big32(cur + 4) & 0xFFFFFF);
			if (offset + 4 > size) {
				sfnt->tables[i].num_entries = 0;
				sfnt->tables[i].data = NULL;
				continue;
			}
			uint32_t res_size = big32(buffer + offset);
			if (offset + 4 + res_size > size || res_size < 0xC) {
				sfnt->tables[i].num_entries = 0;
				sfnt->tables[i].data = NULL;
				continue;
			}
			sfnt->tables[i].container = sfnt;
			sfnt->tables[i].data = buffer + offset + 4 + 0xC;
			sfnt->tables[i].num_entries = big16(buffer + offset + 4 + 4);
			sfnt->tables[i].offset = offset + 4;
		}
		break;
	}
	}
	return sfnt;
}

uint8_t *sfnt_find_table(sfnt_table *sfnt, char *table, uint32_t *size_out)
{
	uint8_t *entry = sfnt->data;
	for (int i = 0; i < sfnt->num_entries; i++, entry += 16)
	{
		if (!strncmp(entry, table, 4)) {
			if (size_out) {
				*size_out = big32(entry + 12);
			}
			return sfnt->container->blob + sfnt->offset + big32(entry + 8);
		}
	}
	return NULL;
}

char *sfnt_name(sfnt_table *sfnt, uint16_t name_type)
{
	uint32_t name_size;
	uint8_t *name_table = sfnt_find_table(sfnt, "name", &name_size);
	if (!name_table) {
		return NULL;
	}
	uint16_t num_names = big16(name_table + 2);
	if ((6 + num_names *12) > name_size) {
		//count is too big for the name table size, abort
		return NULL;
	}
	uint8_t *entry = name_table + 6;
	uint16_t name_length = 0, name_offset;
	uint8_t *unicode_entry = NULL, *macroman_entry = NULL, *winunicode_entry = NULL;
	for (uint16_t i = 0; i < num_names; i++, entry += 12)
	{
		if (big16(entry + 6) != name_type) {
			continue;
		}
		uint16_t language_id = big16(entry + 4);
		if (language_id >= 0x8000) {
			//ingore language tag records
			continue;
		}
		uint16_t platform_id = big16(entry);
		if (platform_id == 0) {
			//prefer Unicode first
			unicode_entry = entry;
			break;
		} else if (platform_id == 3 && big16(entry + 2) < 2) {
			if (!winunicode_entry || (language_id & 0xFF) == 0x09) {
				winunicode_entry = entry;
			}
		} else if (platform_id == 1 && big16(entry + 2) == 0) {
			if (!macroman_entry || (language_id == 0)) {
				macroman_entry = entry;
			}
		}
	}
	entry = unicode_entry ? unicode_entry : winunicode_entry ? winunicode_entry : macroman_entry;
	if (entry) {
		name_length = big16(entry + 8);
		name_offset = big16(entry + 10);
	}
	if (!name_length) {
		return NULL;
	}
	uint32_t full_off = name_offset + big16(name_table + 4);
	if ((full_off + name_length) > name_size) {
		return NULL;
	}
	if (entry == macroman_entry) {
		//TODO: convert these properly to UTF-8
		char *ret = malloc(name_length + 1);
		memcpy(ret, name_table + full_off, name_length);
		ret[name_length] = 0;
		return ret;
	} else {
		return utf16be_to_utf8(name_table + full_off, name_length/2);
	}
}

uint8_t *sfnt_flatten(sfnt_table *sfnt, uint32_t *size_out)
{
	uint8_t *ret = NULL;;
	sfnt_container *cont = sfnt->container;
	switch(cont->container_type)
	{
	case CONTAINER_TTF:
		ret = cont->blob;
		if (size_out) {
			*size_out = cont->size;
		}
		break;
	case CONTAINER_TTC:
		memmove(cont->blob, sfnt->data - 0xC, 0xC + sfnt->num_entries * 12);
		ret = cont->blob;
		if (size_out) {
			*size_out = cont->size;
		}
		break;
	case CONTAINER_DFONT:{
		uint8_t * start = sfnt->data - 0xC;
		uint32_t size = big32(start - 4);
		if (size + (start-cont->blob) > cont->size) {
			size = cont->size - (start-cont->blob);
		}
		ret = malloc(size);
		memcpy(ret, start, size);
		free(cont->blob);
		if (size_out) {
			*size_out = size;
		}
		break;
	}
	}
	free(cont->tables);
	free(cont);
	return ret;
}

sfnt_table *sfnt_subfamily_by_names(sfnt_container *sfnt, const char **names)
{
	for (int i = 0; i < sfnt->num_fonts; i++)
	{
		for (const char **name = names; *name; name++)
		{
			char *font_subfam = sfnt_name(sfnt->tables + i, SFNT_SUBFAMILY);
			if (font_subfam && !strcasecmp(*name, font_subfam)) {
				free(font_subfam);
				return sfnt->tables + i;
			}
			free(font_subfam);
		}
	}
	return NULL;
}

void sfnt_free(sfnt_container *sfnt)
{
	free(sfnt->tables);
	free(sfnt->blob);
	free(sfnt);
}
