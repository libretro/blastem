/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "tern.h"
#include <stddef.h>
#include <stdlib.h>

tern_node * tern_insert(tern_node * head, char * key, tern_val value)
{
	tern_node ** cur = &head;
	while(*key)
	{
		if (*cur) {
			while(*cur && (*cur)->el != *key)
			{
				if (*key < (*cur)->el) {
					cur = &(*cur)->left;
				} else {
					cur = &(*cur)->right;
				}
			}
		}
		if (!*cur) {
			*cur = malloc(sizeof(tern_node));
			(*cur)->left = NULL;
			(*cur)->right = NULL;
			(*cur)->straight.next = NULL;
			(*cur)->el = *key;
		}
		cur = &((*cur)->straight.next);
		key++;
	}
	while(*cur && (*cur)->el)
	{
		cur = &(*cur)->left;
	}
	if (!*cur) {
		*cur = malloc(sizeof(tern_node));
		(*cur)->left = NULL;
		(*cur)->right = NULL;
		(*cur)->el = 0;
	}
	(*cur)->straight.value = value;
	return head;
}

int tern_find(tern_node * head, char * key, tern_val *ret)
{
	tern_node * cur = head;
	while (cur)
	{
		if (cur->el == *key) {
			if (*key) {
				cur = cur->straight.next;
				key++;
			} else {
				*ret = cur->straight.value;
				return 1;
			}
		} else if (*key < cur->el) {
			cur = cur->left;
		} else {
			cur = cur->right;
		}
	}
	return 0;
}

tern_node * tern_find_prefix(tern_node * head, char * key)
{
	tern_node * cur = head;
	while (cur && *key)
	{
		if (cur->el == *key) {
			cur = cur->straight.next;
			key++;
		} else if (*key < cur->el) {
			cur = cur->left;
		} else {
			cur = cur->right;
		}
	}
	return cur;
}

intptr_t tern_find_int(tern_node * head, char * key, intptr_t def)
{
	tern_val ret;
	if (tern_find(head, key, &ret)) {
		return ret.intval;
	}
	return def;
}

tern_node * tern_insert_int(tern_node * head, char * key, intptr_t value)
{
	tern_val val;
	val.intval = value;
	return tern_insert(head, key, val);
}

void * tern_find_ptr(tern_node * head, char * key)
{
	tern_val ret;
	if (tern_find(head, key, &ret)) {
		return ret.ptrval;
	}
	return NULL;
}

tern_node * tern_insert_ptr(tern_node * head, char * key, void * value)
{
	tern_val val;
	val.ptrval = value;
	return tern_insert(head, key, val);
}


