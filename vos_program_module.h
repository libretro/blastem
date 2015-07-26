#ifndef VOS_PROGRAM_MODULE_H_
#define VOS_PROGRAM_MODULE_H_

#include <stdint.h>

typedef struct
{
	struct {
		uint16_t len;
		char     str[32];
	} name;
	uint16_t unknown[5];
	uint32_t code_address;
	uint32_t code_length;
	uint32_t foo_address;
	uint32_t foo_length;
	uint32_t bar_address;
	uint32_t bar_length;
	uint16_t unknown2[3];
} vos_module_map_entry;

typedef struct
{
	struct {
		uint16_t len;
		char     str[32];
	} name;
	uint32_t address;
	uint16_t unknown[3];
} vos_external_var_entry;

typedef struct
{
	uint16_t version;
	struct {
		uint16_t len;
		char     str[32];
	} binder_version;
	struct {
		uint16_t len;
		char     str[32];
	} binder_options;
	struct {
		uint16_t len;
		char     str[32];
	} system_name;
	struct {
		uint16_t len;
		char     str[65];
	} user_name;
	uint32_t date_bound;
	struct {
		uint32_t code_address;
		uint32_t static_address;
	} main_entry_link;
	uint32_t user_boundary;
	uint16_t n_modules;
	uint16_t n_external_vars;
	uint16_t n_link_names;
	uint16_t n_unsnapped_links;
	uint16_t n_vm_pages;
	uint16_t n_header_pages;
	struct {
		uint32_t address;
		uint32_t len;
	} info[3][4];
	uint32_t module_map_address;
	uint32_t module_map_len;
	uint32_t external_vars_map_address;
	uint32_t external_vars_map_len;
	uint32_t link_names_map_address;
	uint32_t link_names_map_len;
	uint32_t link_map_address;
	uint32_t link_map_len;
	uint32_t header_address;
	uint32_t header_len;
	uint8_t  access_info[2048];
	uint32_t flags;
	uint16_t n_tasks;
	uint32_t task_static_len[3];
	uint32_t stack_len;
	uint16_t n_entries;
	uint32_t entry_map_address;
	uint32_t entry_map_len;
	uint16_t pop_version;
	uint16_t processor;
	struct {
		uint16_t len;
		char     str[32];
	} release_name;
	struct {
		uint32_t map_address;
		uint32_t map_len;
		uint32_t n_relocations;
	} relocation_info;
	uint32_t high_water_mark;
	struct {
		uint16_t len;
		char     str[256];
	} copyright_notice;
	uint32_t module_origins[14];
	uint16_t processor_family;
	struct {
		uint16_t len;
		char     str[32];
	} program_name;
	uint32_t string_pool_address;
	uint32_t string_pool_len;
	uint32_t obj_dir_map_address;
	uint32_t obj_dir_map_len;
	uint32_t global_offset_table_address[3];
	struct {
		uint32_t address;
		uint32_t len;
	} block_map_info[3];
	uint32_t section_map_file_address;
	uint32_t section_map_address;
	uint32_t section_map_len;
	uint16_t n_sections;
	uint32_t max_heap_size;
	uint32_t max_program_size;
	uint32_t max_stack_size;
	uint32_t stack_fence_size;

	vos_module_map_entry   *module_map_entries;
	vos_external_var_entry *external_vars;
} vos_program_module;

int vos_read_header(FILE * f, vos_program_module *out);
int vos_read_alloc_module_map(FILE * f, vos_program_module *header);
int vos_read_alloc_external_vars(FILE * f, vos_program_module *header);
void vos_header_cleanup(vos_program_module *header);

#endif //VOS_PROGRAM_MODULE_H_
