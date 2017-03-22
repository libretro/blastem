/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm.
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "tern.h"
#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include "util.h"

tern_node * tern_insert(tern_node * head, char const * key, tern_val value)
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

int tern_find(tern_node * head, char const * key, tern_val *ret)
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

tern_node * tern_find_prefix(tern_node * head, char const * key)
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

intptr_t tern_find_int(tern_node * head, char const * key, intptr_t def)
{
	tern_val ret;
	if (tern_find(head, key, &ret)) {
		return ret.intval;
	}
	return def;
}

tern_node * tern_insert_int(tern_node * head, char const * key, intptr_t value)
{
	tern_val val;
	val.intval = value;
	return tern_insert(head, key, val);
}

void * tern_find_ptr_default(tern_node * head, char const * key, void * def)
{
	tern_val ret;
	if (tern_find(head, key, &ret)) {
		if (ret.intval & 1) {
			return (void *)(ret.intval & ~1);
		} else {
			return ret.ptrval;
		}
	}
	return def;
}

void * tern_find_ptr(tern_node * head, char const * key)
{
	return tern_find_ptr_default(head, key, NULL);
}

tern_val tern_find_path_default(tern_node *head, char const *key, tern_val def)
{
	tern_val ret;
	while (*key)
	{
		if (!tern_find(head, key, &ret)) {
			return def;
		}
		key = key + strlen(key) + 1;
		if (*key) {
			head = tern_get_node(ret);
			if (!head) {
				return def;
			}
		}
	}
	return ret;
}

tern_val tern_find_path(tern_node *head, char const *key)
{
	tern_val def;
	def.ptrval = NULL;
	return tern_find_path_default(head, key, def);
}

tern_node * tern_insert_ptr(tern_node * head, char const * key, void * value)
{
	tern_val val;
	val.ptrval = value;
	return tern_insert(head, key, val);
}

tern_node * tern_insert_node(tern_node *head, char const *key, tern_node *value)
{
	tern_val val;
	val.intval = ((intptr_t)value) | 1;
	return tern_insert(head, key, val);
}

uint32_t tern_count(tern_node *head)
{
	uint32_t count = 0;
	if (head->left) {
		count += tern_count(head->left);
	}
	if (head->right) {
		count += tern_count(head->right);
	}
	if (!head->el) {
		count++;
	} else if (head->straight.next) {
		count += tern_count(head->straight.next);
	}
	return count;
}

#define MAX_ITER_KEY 127
void tern_foreach_int(tern_node *head, iter_fun fun, void *data, char *keybuf, int pos)
{
	if (!head->el) {
		keybuf[pos] = 0;
		fun(keybuf, head->straight.value, data);
	}
	if (head->left) {
		tern_foreach_int(head->left, fun, data, keybuf, pos);
	}
	if (head->el) {
		if (pos == MAX_ITER_KEY) {
			fatal_error("tern_foreach_int: exceeded maximum key size");
		}
		keybuf[pos] = head->el;
		tern_foreach_int(head->straight.next, fun, data, keybuf, pos+1);
	}
	if (head->right) {
		tern_foreach_int(head->right, fun, data, keybuf, pos);
	}
}

void tern_foreach(tern_node *head, iter_fun fun, void *data)
{
	//lame, but good enough for my purposes
	char key[MAX_ITER_KEY+1];
	tern_foreach_int(head, fun, data, key, 0);
}

char * tern_int_key(uint32_t key, char * buf)
{
	char * cur = buf;
	while (key)
	{
		*(cur++) = (key & 0x7F) + 1;
		key >>= 7;
	}
	*cur = 0;
	return buf;
}

tern_node * tern_get_node(tern_val value)
{
	return value.intval & 1 ? (tern_node *)(value.intval & ~1) : NULL;
}

void tern_free(tern_node *head)
{
	if (head->left) {
		tern_free(head->left);
	}
	if (head->right) {
		tern_free(head->right);
	}
	if (head->el) {
		tern_free(head->straight.next);
	}
}
