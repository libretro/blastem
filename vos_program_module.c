#include <stdio.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include "vos_program_module.h"

static uint16_t big16(uint8_t ** src)
{
	uint16_t ret = *((*src)++) << 8;
	ret |= *((*src)++);
	return ret;
}

static uint32_t big32(uint8_t ** src)
{
	uint32_t ret = *((*src)++) << 24;
	ret |= *((*src)++) << 16;
	ret |= *((*src)++) << 8;
	ret |= *((*src)++);
	return ret;
}

static void string_(uint8_t ** src, uint16_t *len, char * str, uint32_t storage)
{
	*len = big16(src);
	memcpy(str, *src, storage);
	*src += storage;
	if (*len >= storage)
	{
		*len = storage;
	} else {
		str[*len] = 0;
	}
	if (storage & 1)
	{
		(*src)++;
	}
}

#define string(src, field) string_(src, &(field).len, (field).str, sizeof((field).str))


int vos_read_header(FILE * f, vos_program_module *out)
{
	uint8_t buffer[4096];
	if (fread(buffer, 1, sizeof(buffer), f) != sizeof(buffer))
	{
		return 0;
	}
	uint8_t *cur = buffer;
	out->version = big16(&cur);
	string(&cur, out->binder_version);
	string(&cur, out->binder_options);
	string(&cur, out->system_name);
	string(&cur, out->user_name);
	out->date_bound = big32(&cur);
	out->main_entry_link.code_address = big32(&cur);
	out->main_entry_link.static_address = big32(&cur);
	out->user_boundary = big32(&cur);
	out->n_modules = big16(&cur);
	out->n_external_vars = big16(&cur);
	out->n_link_names = big16(&cur);
	out->n_unsnapped_links = big16(&cur);
	out->n_vm_pages = big16(&cur);
	out->n_header_pages = big16(&cur);
	for (int i = 0; i < 3; i++)
	{
		for (int j = 0; j < 4; j++)
		{
			out->info[i][j].address = big32(&cur);
			out->info[i][j].len = big32(&cur);
		}
	}
	out->module_map_address = big32(&cur);
	out->module_map_len = big32(&cur);
	out->external_vars_map_address = big32(&cur);
	out->external_vars_map_len = big32(&cur);
	out->link_names_map_address = big32(&cur);
	out->link_names_map_len = big32(&cur);
	out->link_map_address = big32(&cur);
	out->link_map_len = big32(&cur);
	out->header_address = big32(&cur);
	out->header_len = big32(&cur);
	memcpy(out->access_info, cur, sizeof(out->access_info));
	cur += sizeof(out->access_info);
	out->flags = big32(&cur);
	out->n_tasks = big16(&cur);
	for (int i = 0; i < 3; i++)
	{
		out->task_static_len[i] = big32(&cur);
	}
	out->stack_len = big32(&cur);
	out->n_entries = big16(&cur);
	out->entry_map_address = big32(&cur);
	out->entry_map_len = big32(&cur);
	out->pop_version = big16(&cur);
	out->processor = big16(&cur);
	string(&cur, out->release_name);
	out->relocation_info.map_address = big32(&cur);
	out->relocation_info.map_len = big32(&cur);
	out->relocation_info.n_relocations = big32(&cur);
	out->high_water_mark = big32(&cur);
	string(&cur, out->copyright_notice);
	for (int i = 0; i < 14; i++)
	{
		out->module_origins[i] = big32(&cur);
	}
	out->processor_family = big16(&cur);
	string(&cur, out->program_name);
	out->string_pool_address = big32(&cur);
	out->string_pool_len = big32(&cur);
	out->obj_dir_map_address = big32(&cur);
	out->obj_dir_map_len = big32(&cur);
	for (int i = 0; i < 3; i++)
	{
		out->global_offset_table_address[i] = big32(&cur);
	}
	for (int i = 0; i < 3; i++)
	{
		out->block_map_info[i].address = big32(&cur);
		out->block_map_info[i].len = big32(&cur);
	}
	out->section_map_file_address = big32(&cur);
	out->section_map_address = big32(&cur);
	out->section_map_len = big32(&cur);
	out->n_sections = big16(&cur);
	out->max_heap_size = big32(&cur);
	out->max_program_size = big32(&cur);
	out->max_stack_size = big32(&cur);
	out->stack_fence_size = big32(&cur);

	out->module_map_entries = NULL;
	out->external_vars = NULL;
	return 1;
}

#define MODULE_MAP_ENTRY_SIZE 74

int vos_read_alloc_module_map(FILE * f, vos_program_module *header)
{
	if (header->module_map_len != header->n_modules * MODULE_MAP_ENTRY_SIZE)
	{
		return 0;
	}
	uint8_t * buf = malloc(header->module_map_len);
	fseek(f, header->module_map_address + 0x1000 - header->user_boundary, SEEK_SET);
	if (fread(buf, 1, header->module_map_len, f) != header->module_map_len)
	{
		free(buf);
		return 0;
	}
	uint8_t * cur = buf;
	header->module_map_entries = malloc(sizeof(vos_module_map_entry) * header->n_modules);
	for (int i = 0; i < header->n_modules; i++)
	{
		string(&cur, header->module_map_entries[i].name);
		for (int j = 0; j < 5; j++)
		{
			header->module_map_entries[i].unknown[j] = big16(&cur);
		}
		header->module_map_entries[i].code_address = big32(&cur);
		header->module_map_entries[i].code_length = big32(&cur);
		header->module_map_entries[i].foo_address = big32(&cur);
		header->module_map_entries[i].foo_length = big32(&cur);
		header->module_map_entries[i].bar_address = big32(&cur);
		header->module_map_entries[i].bar_length = big32(&cur);
		for (int j = 0; j < 3; j++)
		{
			header->module_map_entries[i].unknown2[j] = big16(&cur);
		}
	}
	return 1;
}

#define EXTERNAL_VAR_ENTRY_SIZE 44

int vos_read_alloc_external_vars(FILE * f, vos_program_module *header)
{
	if (header->external_vars_map_len != header->n_external_vars * EXTERNAL_VAR_ENTRY_SIZE)
	{
		return 0;
	}
	uint8_t * buf = malloc(header->external_vars_map_len);
	fseek(f, header->external_vars_map_address + 0x1000 - header->user_boundary, SEEK_SET);
	if (fread(buf, 1, header->external_vars_map_len, f) != header->external_vars_map_len)
	{
		free(buf);
		return 0;
	}
	uint8_t * cur = buf;
	header->external_vars = malloc(sizeof(vos_external_var_entry) * header->n_external_vars);
	for (int i = 0; i < header->n_external_vars; i++)
	{
		string(&cur, header->external_vars[i].name);
		header->external_vars[i].address = big32(&cur);
		for (int j = 0; j < 3; j++)
		{
			header->external_vars[i].unknown[j] = big16(&cur);
		}
	}
	return 1;
}

void vos_header_cleanup(vos_program_module *header)
{
	free(header->module_map_entries);
	free(header->external_vars);
}
