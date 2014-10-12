#include <stdio.h>
#include "vos_program_module.h"

int main(int argc, char ** argv)
{
	vos_program_module header;
	FILE * f = fopen(argv[1], "rb");
	vos_read_header(f, &header);
	vos_read_alloc_module_map(f, &header);
	vos_read_alloc_external_vars(f, &header);

	printf("Version: %d\n", header.version);
	printf("Binder Version: %s\n", header.binder_version.str);
	printf("Binder Options: %s\n", header.binder_options.str);
	printf("System name: %s\n", header.system_name.str);
	printf("User name: %s\n", header.user_name.str);
	printf("Date bound: %d\n", header.date_bound);
	printf("Code addresss: 0x%X, Static address: 0x%X\n",
	       header.main_entry_link.code_address, header.main_entry_link.static_address);
	printf("User boundary: 0x%X\n", header.user_boundary);
	printf("Num modules: %d\n", header.n_modules);
	printf("Num extern vars: %d\n", header.n_external_vars);
	printf("Num link names: %d\n", header.n_link_names);
	printf("Num unsapped links: %d\n", header.n_unsnapped_links);
	printf("Num VM pages: %d\n", header.n_vm_pages);
	printf("Num header pages: %d\n", header.n_header_pages);
	for (int i = 0; i < 3; i++) {
		for (int j = 0; j < 4; j++) {
			printf("Info %d:%d\n\tAddress: 0x%X\n\tLength: 0x%X\n",
			       i, j, header.info[i][j].address, header.info[i][j].len);
		}
	}
	printf("Module map address: 0x%X\n", header.module_map_address);
	printf("Module map length: 0x%X\n", header.module_map_len);
	printf("External vars map address: 0x%X\n", header.external_vars_map_address);
	printf("External vars map length: 0x%X\n", header.external_vars_map_len);
	printf("Link names map address: 0x%X\n", header.link_names_map_address);
	printf("Link names map length: 0x%X\n", header.link_names_map_len);
	printf("Header address: 0x%X\n", header.header_address);
	printf("Header length: 0x%X\n", header.header_len);
	//printf("Access Info: 0x%X\n", header.header_address);
	printf("Flags: 0x%X\n", header.flags);
	printf("Num tasks: %d\n", header.n_tasks);
	printf("Stack Size: 0x%X\n", header.stack_len);
	printf("Num entries: %d\n", header.n_entries);
	printf("Entry map address: 0x%X\n", header.entry_map_address);
	printf("Entry map length: 0x%X\n", header.entry_map_len);
	printf("Pop Version: %d\n", header.pop_version);
	printf("Processor: %d\n", header.processor);
	printf("Processor family: %d\n", header.processor_family);
	printf("Release name: %s\n", header.release_name.str);
	printf("Relocation info:\n\tMap Addres: 0x%X\n\tMap Length: 0x%X\n\tNum Relocations: %d\n",
	       header.relocation_info.map_address, header.relocation_info.map_len,
		   header.relocation_info.n_relocations);
	printf("High water mark: 0x%X\n", header.high_water_mark);
	printf("Copyright notice: %s\n", header.program_name.str);
	printf("String pool address: 0x%X\n", header.string_pool_address);
	printf("String pool length: 0x%X\n", header.string_pool_len);
	printf("Object dir map address: 0x%X\n", header.obj_dir_map_address);
	printf("Object dir map length: 0x%X\n", header.obj_dir_map_len);
	puts("Global offset table addresses:");
	for (int i = 0; i < 3; i++) {
		printf("\t%d: 0x%X\n", i, header.global_offset_table_address[i]);
	}
	for (int i = 0; i < 3; i++) {
		printf("Block map info %d\n\tAddress: 0x%X\n\tLength: 0x%X\n",
			   i, header.block_map_info[i].address, header.block_map_info[i].len);
	}
	printf("Secton map file address: 0x%X\n", header.section_map_file_address);
	printf("Secton map address: 0x%X\n", header.section_map_address);
	printf("Secton map length: 0x%X\n", header.section_map_len);
	printf("Num sections: %d\n", header.n_sections);
	printf("Max heap size: 0x%X\n", header.max_heap_size);
	printf("Max program size: 0x%X\n", header.max_program_size);
	printf("Max stack size: 0x%X\n", header.max_stack_size);
	printf("Stack fence size: 0x%X\n", header.stack_fence_size);

	puts("\nModules");
	for (int i = 0; i < header.n_modules; i++) {
		printf("\t%s:\n\t\tCode Address: 0x%X, Length: 0x%X\n",
			   header.module_map_entries[i].name.str,
			   header.module_map_entries[i].code_address,
			   header.module_map_entries[i].code_length);
		printf("\t\tFoo Address: 0x%X, Length: 0x%X\n",
		       header.module_map_entries[i].foo_address,
			   header.module_map_entries[i].foo_length);
		printf("\t\tBar Address: 0x%X, Length: 0x%X\n",
		       header.module_map_entries[i].bar_address,
			   header.module_map_entries[i].bar_length);
	}

	puts("\nExtrnal Vars");
	for (int i = 0; i < header.n_external_vars; i++) {
		printf("\t%s: 0x%X\n",
		       header.external_vars[i].name.str, header.external_vars[i].address);
	}

	vos_header_cleanup(&header);
	return 0;
}
