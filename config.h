/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef CONFIG_H_
#define CONFIG_H_
#include "tern.h"

tern_node * parse_config_file(char * config_path);
tern_node * load_config();
#ifdef __ANDROID__
tern_node * parse_config_file_assets(char *config_path);
#endif

#endif //CONFIG_H_

