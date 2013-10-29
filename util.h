#ifndef UTIL_H_
#define UTIL_H_

#include <stdio.h>

//Utility functions

//Allocates a new string containing the concatenation of first and second
char * alloc_concat(char * first, char * second);
//Allocates a new string containing the concatenation of the strings pointed to by parts
char * alloc_concat_m(int num_parts, char ** parts);
//Returns the size of a file using fseek and ftell
long file_size(FILE * f);
//Strips whitespace and non-printable characters from the beginning and end of a string
char * strip_ws(char * text);
//Inserts a null after the first word, returns a pointer to the second word
char * split_keyval(char * text);
//Should be called by main with the value of argv[0] for use by get_exe_dir
void set_exe_str(char * str);
//Returns the directory the executable is in
char * get_exe_dir();
//Returns the contents of a symlink in a newly allocated string
char * readlink_alloc(char * path);

#endif //UTIL_H_
