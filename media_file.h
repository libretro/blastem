#ifndef MEDIA_FILE_H_
#define MEDIA_FILE_H_

/*
 How a CD image, one of the track files a cue sheet names, or a BIOS gets read.

 Plain stdio everywhere except the libretro build, which routes it through the
 frontend's VFS so paths only the frontend can open - Android content:// URIs
 from the Storage Access Framework, files on an SMB share - work too. See
 vfs_file.h; with no VFS interface on offer that layer is stdio again, so this
 indirection costs nothing.
*/

#include <stdio.h>

#ifdef IS_LIB

#include "vfs_file.h"

typedef vfs_file media_file;
#define media_fopen     vfs_fopen
#define media_fread     vfs_fread
#define media_fseek     vfs_fseek
#define media_ftell     vfs_ftell
#define media_fgetc     vfs_fgetc
#define media_fclose    vfs_fclose
#define media_file_size vfs_file_size
#define media_path_is_external vfs_path_is_external

#else

typedef FILE media_file;
#ifdef __ANDROID__
FILE* fopen_wrapper(const char *path, const char *mode);
#define media_fopen fopen_wrapper
#else
#define media_fopen fopen
#endif
#define media_fread     fread
#define media_fseek     fseek
#define media_ftell     ftell
#define media_fgetc     fgetc
#define media_fclose    fclose
#define media_file_size file_size
#define media_path_is_external(path) is_absolute_path(path)

#endif //IS_LIB

#endif //MEDIA_FILE_H_
