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
char * split_keyval(char * text);

#endif //UTIL_H_
