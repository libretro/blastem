/*
 Copyright 2013 Michael Pavone
 This file is part of BlastEm. 
 BlastEm is free software distributed under the terms of the GNU General Public License version 3 or greater. See COPYING for full license text.
*/
#include "tern.h"
#include <stdio.h>
#include <stddef.h>

int main(int argc, char ** argv)
{
	tern_node * tree = tern_insert_ptr(NULL, "foo", "bar");
	tree = tern_insert_ptr(tree, "foobar", "baz");
	tree = tern_insert_ptr(tree, "goobar", "qux");
	tree = tern_insert_int(tree, "foobarbaz", 42);
	tree = tern_insert_int(tree, "goobarbaz", 21);
	printf("foo: %s\n", (char *)tern_find_ptr(tree, "foo"));
	printf("foobar: %s\n", (char *)tern_find_ptr(tree, "foobar"));
	printf("goobar: %s\n", (char *)tern_find_ptr(tree, "goobar"));
	printf("foob: %s\n", (char *)tern_find_ptr(tree, "foob"));
	printf("foobarbaz: %d\n", (int)tern_find_int(tree, "foobarbaz", 0));
	printf("goobarbaz: %d\n", (int)tern_find_int(tree, "goobarbaz", 0));
	printf("foobarb: %d\n", (int)tern_find_int(tree, "foobarb", 0));
	return 0;
}

