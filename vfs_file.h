#ifndef VFS_FILE_H_
#define VFS_FILE_H_

/*
 A thin "read a media file" layer for the libretro build.

 The frontend may hand us paths we cannot open ourselves: Android content://
 URIs from the Storage Access Framework, SMB shares, or anything else behind
 RETRO_ENVIRONMENT_GET_VFS_INTERFACE. ROMs listed in the content info override
 arrive in memory and never reach this code, but CD images (cue/toc/iso/chd,
 plus every track file a cue sheet names), compressed ROMs and the Sega CD /
 32X BIOS are all opened by us, by path.

 So everything that opens a media file by name goes through here instead of
 stdio. With no VFS interface offered - an older frontend, or the standalone
 emulator - it is plain stdio, which is also why blastem's own code outside
 the libretro build is left alone.

 Gzip is handled here too. The stdio path used to get it from gzopen(), and
 that takes a filename, so it cannot be pointed at a VFS handle; a gzipped
 file is instead read through the VFS and inflated into memory up front.
*/

#include <stddef.h>
#include <stdint.h>

typedef struct vfs_file vfs_file;

struct retro_vfs_interface;

//Called once from retro_set_environment. iface may be NULL, which keeps
//everything on stdio.
void vfs_file_set_interface(struct retro_vfs_interface *iface, uint32_t version);

vfs_file *vfs_fopen(const char *path, const char *mode);
size_t vfs_fread(void *dst, size_t size, size_t count, vfs_file *f);
int vfs_fseek(vfs_file *f, long offset, int whence);
long vfs_ftell(vfs_file *f);
int vfs_fgetc(vfs_file *f);
int vfs_fclose(vfs_file *f);
//Size of the file in bytes, or of the inflated data for a gzipped one.
long vfs_file_size(vfs_file *f);
//Whether a path points outside the emulator directory - absolute, or a URI
//the frontend's VFS can resolve.
int vfs_path_is_external(const char *path);
//Whether the file is being read as-is, i.e. gzip did not kick in. Mirrors
//zlib's gzdirect(), which the ROM loader uses to spot a gzipped ROM.
int vfs_fdirect(vfs_file *f);

#endif //VFS_FILE_H_
