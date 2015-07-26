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
//Gets the smallest power of two that is >= a certain value, won't work for values > 0x80000000
uint32_t nearest_pow2(uint32_t val);
//Should be called by main with the value of argv[0] for use by get_exe_dir
void set_exe_str(char * str);
//Returns the directory the executable is in
char * get_exe_dir();
//Returns the user's home directory
char * get_home_dir();
//Returns the contents of a symlink in a newly allocated string
char * readlink_alloc(char * path);
//Prints an error message to stderr and to a message box if not in headless mode and then exits
void fatal_error(char *format, ...);
//Prints an information message to stdout and to a message box if not in headless mode and not attached to a console
void info_message(char *format, ...);
//Prints an information message to stderr and to a message box if not in headless mode and not attached to a console
void warning(char *format, ...);

#endif //UTIL_H_
