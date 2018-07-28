/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef CONFIG_H_
#define CONFIG_H_
#include "tern.h"

tern_node *parse_config_file(char *config_path);
tern_node *parse_bundled_config(char *config_name);
tern_node *load_overrideable_config(char *name, char *bundled_name);
tern_node *load_config();
char *serialize_config(tern_node *config, uint32_t *size_out);
uint8_t serialize_config_file(tern_node *config, char *path);
void persist_config_at(tern_node *config, char *fname);
void persist_config(tern_node *config);
char **get_extension_list(tern_node *config, uint32_t *num_exts_out);
uint32_t get_lowpass_cutoff(tern_node *config);

#endif //CONFIG_H_

