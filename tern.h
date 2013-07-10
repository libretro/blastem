#ifndef TERN_H_
#define TERN_H_

#include <stdint.h>

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

tern_node * tern_insert(tern_node * head, char * key, tern_val value);
int tern_find(tern_node * head, char * key, tern_val *ret);
intptr_t tern_find_int(tern_node * head, char * key, intptr_t def);
tern_node * tern_insert_int(tern_node * head, char * key, intptr_t value);
void * tern_find_ptr(tern_node * head, char * key);
tern_node * tern_insert_ptr(tern_node * head, char * key, void * value);

#endif //TERN_H_
