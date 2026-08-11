#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "vfs_file.h"
#include "libretro.h"
#include "paths.h"
#ifndef DISABLE_ZLIB
#include "zlib/zlib.h"
#endif

static struct retro_vfs_interface *vfs;
static uint32_t vfs_version;

void vfs_file_set_interface(struct retro_vfs_interface *iface, uint32_t version)
{
	vfs = iface;
	vfs_version = version;
}

struct vfs_file {
	struct retro_vfs_file_handle *vf;	//VFS handle, when the frontend gave us one
	FILE *fp;							//plain stdio otherwise
	uint8_t *mem;						//inflated contents, for a gzipped file
	long mem_size;
	long mem_pos;
};

#ifndef DISABLE_ZLIB
//Read the whole file and inflate it. Returns 0 and leaves the file positioned
//at the start again if it turns out not to be usable gzip data, so the caller
//can carry on reading it as-is.
static int slurp_gzip(vfs_file *f)
{
	long size = vfs_file_size(f);
	if (size <= 0) {
		return 0;
	}
	uint8_t *raw = malloc(size);
	if (!raw) {
		return 0;
	}
	vfs_fseek(f, 0, SEEK_SET);
	if (vfs_fread(raw, 1, size, f) != (size_t)size) {
		free(raw);
		vfs_fseek(f, 0, SEEK_SET);
		return 0;
	}

	z_stream z;
	memset(&z, 0, sizeof(z));
	//16 + MAX_WBITS: decode a gzip wrapper rather than a raw zlib stream
	if (inflateInit2(&z, 16 + MAX_WBITS) != Z_OK) {
		free(raw);
		vfs_fseek(f, 0, SEEK_SET);
		return 0;
	}
	z.next_in = raw;
	z.avail_in = size;

	long cap = size * 4 + 4096;
	uint8_t *out = malloc(cap);
	long have = 0;
	int status;
	do {
		if (have == cap) {
			cap *= 2;
			uint8_t *bigger = realloc(out, cap);
			if (!bigger) {
				free(out);
				out = NULL;
				break;
			}
			out = bigger;
		}
		z.next_out = out + have;
		z.avail_out = cap - have;
		status = inflate(&z, Z_NO_FLUSH);
		have = cap - z.avail_out;
	} while (status == Z_OK && out);
	inflateEnd(&z);
	free(raw);

	//Only a complete stream counts. Anything else - a truncated file, or data
	//that merely happens to start with the gzip magic - is read as-is instead.
	if (!out || status != Z_STREAM_END || !have) {
		free(out);
		vfs_fseek(f, 0, SEEK_SET);
		return 0;
	}
	f->mem = out;
	f->mem_size = have;
	f->mem_pos = 0;
	return 1;
}
#endif //DISABLE_ZLIB

//mode is the stdio one and only reaches the stdio path; media is opened for
//reading only, which is all RETRO_VFS_FILE_ACCESS_READ offers.
vfs_file *vfs_fopen(const char *path, const char *mode)
{
	vfs_file *f = calloc(1, sizeof(vfs_file));
	if (!f) {
		return NULL;
	}
	if (vfs) {
		f->vf = vfs->open(path, RETRO_VFS_FILE_ACCESS_READ, RETRO_VFS_FILE_ACCESS_HINT_NONE);
	} else {
		f->fp = fopen(path, mode);
	}
	if (!f->vf && !f->fp) {
		free(f);
		return NULL;
	}
#ifndef DISABLE_ZLIB
	uint8_t magic[2];
	if (vfs_fread(magic, 1, sizeof(magic), f) == sizeof(magic) && magic[0] == 0x1F && magic[1] == 0x8B) {
		slurp_gzip(f);
	}
	if (!f->mem) {
		vfs_fseek(f, 0, SEEK_SET);
	}
#endif
	return f;
}

size_t vfs_fread(void *dst, size_t size, size_t count, vfs_file *f)
{
	size_t bytes = size * count;
	if (!bytes) {
		return 0;
	}
	if (f->mem) {
		long left = f->mem_size - f->mem_pos;
		if (left <= 0) {
			return 0;
		}
		if (bytes > (size_t)left) {
			bytes = left;
		}
		memcpy(dst, f->mem + f->mem_pos, bytes);
		f->mem_pos += bytes;
		return bytes / size;
	}
	if (f->vf) {
		int64_t got = vfs->read(f->vf, dst, bytes);
		return got > 0 ? (size_t)got / size : 0;
	}
	return fread(dst, size, count, f->fp);
}

int vfs_fseek(vfs_file *f, long offset, int whence)
{
	if (f->mem) {
		long base = whence == SEEK_CUR ? f->mem_pos : (whence == SEEK_END ? f->mem_size : 0);
		long pos = base + offset;
		if (pos < 0 || pos > f->mem_size) {
			return -1;
		}
		f->mem_pos = pos;
		return 0;
	}
	if (f->vf) {
		int seek_pos = whence == SEEK_CUR ? RETRO_VFS_SEEK_POSITION_CURRENT
		             : (whence == SEEK_END ? RETRO_VFS_SEEK_POSITION_END : RETRO_VFS_SEEK_POSITION_START);
		return vfs->seek(f->vf, offset, seek_pos) < 0 ? -1 : 0;
	}
	return fseek(f->fp, offset, whence);
}

long vfs_ftell(vfs_file *f)
{
	if (f->mem) {
		return f->mem_pos;
	}
	if (f->vf) {
		return (long)vfs->tell(f->vf);
	}
	return ftell(f->fp);
}

int vfs_fgetc(vfs_file *f)
{
	uint8_t byte;
	return vfs_fread(&byte, 1, 1, f) == 1 ? byte : -1;
}

int vfs_fclose(vfs_file *f)
{
	int ret = 0;
	if (f->vf) {
		ret = vfs->close(f->vf);
	} else if (f->fp) {
		ret = fclose(f->fp);
	}
	free(f->mem);
	free(f);
	return ret;
}

long vfs_file_size(vfs_file *f)
{
	if (f->mem) {
		return f->mem_size;
	}
	if (f->vf) {
		return (long)vfs->size(f->vf);
	}
	long cur = ftell(f->fp);
	fseek(f->fp, 0, SEEK_END);
	long size = ftell(f->fp);
	fseek(f->fp, cur, SEEK_SET);
	return size;
}

int vfs_fdirect(vfs_file *f)
{
	return f->mem == NULL;
}

//Whether a path names something outside the emulator's own directory. A VFS
//path does not have to look absolute to be one: Android hands out content://
//URIs and an SMB share is smb://, and treating those as relative sends the
//caller to read_bundled_file(), which in a libretro build finds nothing.
int vfs_path_is_external(const char *path)
{
	if (is_absolute_path((char *)path)) {
		return 1;
	}
	return vfs && strstr(path, "://") != NULL;
}
