/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#ifndef TERN_H_
#define TERN_H_

#include <stdint.h>

#define MAX_INT_KEY_SIZE (sizeof(uint32_t) + 2)

typedef union {
	void     *ptrval;
	intptr_t intval;
} tern_val;

typedef struct tern_node {
	struct tern_node *left;
	union {
		struct tern_node *next;
		tern_val         value;
	} straight;
	struct tern_node *right;
	char             el;
} tern_node;

typedef void (*iter_fun)(char *key, tern_val val, void *data);

tern_node * tern_insert(tern_node * head, char const * key, tern_val value);
int tern_find(tern_node * head, char const * key, tern_val *ret);
tern_node * tern_find_prefix(tern_node * head, char const * key);
intptr_t tern_find_int(tern_node * head, char const * key, intptr_t def);
tern_node * tern_insert_int(tern_node * head, char const * key, intptr_t value);
void * tern_find_ptr_default(tern_node * head, char const * key, void * def);
void * tern_find_ptr(tern_node * head, char const * key);
tern_val tern_find_path_default(tern_node *head, char const *key, tern_val def);
tern_val tern_find_path(tern_node *head, char const *key);
tern_node * tern_insert_ptr(tern_node * head, char const * key, void * value);
tern_node * tern_insert_node(tern_node *head, char const *key, tern_node *value);
uint32_t tern_count(tern_node *head);
void tern_foreach(tern_node *head, iter_fun fun, void *data);
char * tern_int_key(uint32_t key, char * buf);
tern_node * tern_get_node(tern_val value);
void tern_free(tern_node *head);

#endif //TERN_H_
