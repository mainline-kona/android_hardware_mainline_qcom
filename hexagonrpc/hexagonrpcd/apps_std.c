/*
 * FastRPC operating system interface implementation
 *
 * Copyright (C) 2023 The Sensor Shell Contributors
 *
 * This file is part of sensh.
 *
 * Sensh is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "aee_error.h"
#include "interfaces/apps_std.def"
#include "hexagonfs.h"
#include "iobuffer.h"
#include "listener.h"

struct apps_std_ctx {
	int rootfd;
	int adsp_avs_cfg_dirfd;
	int adsp_library_dirfd;
	struct hexagonfs_fd *fds[HEXAGONFS_MAX_FD];
};

static const int apps_std_whence_table[] = {
	SEEK_SET,
	SEEK_CUR,
	SEEK_END,
};

/*
 * This is a placeholder function used to complete any I/O operations.
 * File descriptors do not have a flush operation because their reads and
 * writes are blocking.
 */
static uint32_t apps_std_fflush(void *data,
				const struct fastrpc_io_buffer *inbufs,
				struct fastrpc_io_buffer *outbufs)
{
#ifdef HEXAGONRPC_VERBOSE
	uint32_t *fd = inbufs[0].p;

	printf("ignore fflush(%u)\n", *fd);
#endif

	memset(outbufs[0].p, 0, outbufs[0].s);

	return 0;
}

static uint32_t apps_std_fclose(void *data,
				const struct fastrpc_io_buffer *inbufs,
				struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const uint32_t *first_in = inbufs[0].p;
	int ret;

	ret = hexagonfs_close(ctx->fds, *first_in);
	if (ret) {
		fprintf(stderr, "Could not close: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("close(%u)\n", *first_in);
#endif

	return 0;
}

static uint32_t apps_std_fread(void *data,
			       const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const struct {
		uint32_t fd;
		uint32_t buf_size;
	} *first_in = inbufs[0].p;
	struct {
		uint32_t written;
		uint32_t is_eof;
	} *first_out = outbufs[0].p;
	ssize_t ret;

	ret = hexagonfs_read(ctx->fds, first_in->fd,
			     first_in->buf_size, outbufs[1].p);
	if (ret < 0) {
		fprintf(stderr, "Could not read file: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("read(%u, %u) -> %ld\n", first_in->fd,
					first_in->buf_size,
					ret);
#endif

	first_out->written = ret;
	first_out->is_eof = first_out->written < first_in->buf_size;

	return 0;
}

static uint32_t apps_std_fseek(void *data,
			       const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const struct {
		uint32_t fd;
		uint32_t pos;
		uint32_t whence;
	} *first_in = inbufs[0].p;
	int ret;
	int whence;

	whence = apps_std_whence_table[first_in->whence];

	ret = hexagonfs_lseek(ctx->fds, first_in->fd, first_in->pos, whence);
	if (ret) {
		fprintf(stderr, "Could not seek stream: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("lseek(%u, %d, %d)\n", first_in->fd,
				      first_in->pos,
				      first_in->whence);
#endif

	return 0;
}

/*
 * Normalize a virtual path and allow write/rename/remove only inside
 * /mnt/vendor/persist/sensors/registry/ (or /persist/sensors/registry/).
 */
static int resolve_persist_registry_path(const char *vpath, char *out, size_t out_sz)
{
	static const char persist_prefix[] = "/mnt/vendor/persist/sensors/registry/";
	static const char short_prefix[] = "/persist/sensors/registry/";
	const char *segs[64];
	size_t lens[64];
	int depth = 0;
	const char *p = vpath;
	char norm[PATH_MAX];
	size_t pos = 0;

	if (!vpath || vpath[0] != '/')
		return -EINVAL;

	while (*p) {
		while (*p == '/')
			p++;
		if (!*p)
			break;
		const char *start = p;
		while (*p && *p != '/')
			p++;
		size_t len = (size_t)(p - start);
		if (len == 1 && start[0] == '.') {
			continue;
		} else if (len == 2 && start[0] == '.' && start[1] == '.') {
			if (depth > 0)
				depth--;
		} else {
			if (depth >= 64)
				return -ENAMETOOLONG;
			segs[depth] = start;
			lens[depth] = len;
			depth++;
		}
	}

	for (int i = 0; i < depth; i++) {
		if (pos + 1 + lens[i] >= sizeof(norm))
			return -ENAMETOOLONG;
		norm[pos++] = '/';
		memcpy(norm + pos, segs[i], lens[i]);
		pos += lens[i];
	}
	norm[pos] = '\0';

	if (!strncmp(norm, persist_prefix, sizeof(persist_prefix) - 1)) {
		if (pos >= out_sz)
			return -ENAMETOOLONG;
		memcpy(out, norm, pos + 1);
		return 0;
	}
	if (!strncmp(norm, short_prefix, sizeof(short_prefix) - 1)) {
		const char *rest = norm + (sizeof(short_prefix) - 1);
		int n = snprintf(out, out_sz, "%s%s", persist_prefix, rest);
		if (n < 0 || (size_t)n >= out_sz)
			return -ENAMETOOLONG;
		return 0;
	}

	return -EACCES;
}

static int mode_to_open_flags(const char *mode, bool *is_write)
{
	bool plus = strchr(mode, '+') != NULL;

	if (mode[0] == 'w') {
		*is_write = true;
		return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_TRUNC;
	}
	if (mode[0] == 'a') {
		*is_write = true;
		return (plus ? O_RDWR : O_WRONLY) | O_CREAT | O_APPEND;
	}
	if (mode[0] == 'r' && plus) {
		*is_write = true;
		return O_RDWR;
	}
	*is_write = false;
	return O_RDONLY;
}

static uint32_t apps_std_fopen(void *data,
			       const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	uint32_t *out = outbufs[0].p;
	const char *name = inbufs[1].p;
	const char *mode = inbufs[2].p;
	bool is_write = false;
	int flags, fd;

	if (name[inbufs[1].s - 1] != 0 || mode[inbufs[2].s - 1] != 0)
		return AEE_EBADPARM;

	flags = mode_to_open_flags(mode, &is_write);
	if (is_write) {
		char phys[PATH_MAX];

		if (resolve_persist_registry_path(name, phys, sizeof(phys)) != 0) {
			fprintf(stderr, "Tried to open %s for writing\n", name);
			return AEE_EUNSUPPORTED;
		}
		fd = hexagonfs_open_phys(ctx->fds, phys, flags, 0660);
		if (fd < 0) {
			fprintf(stderr, "Could not open %s for writing: %s\n",
				phys, strerror(-fd));
			return AEE_EFAILED;
		}
		*out = fd;
		return 0;
	}

	fd = hexagonfs_openat(ctx->fds, ctx->rootfd, ctx->rootfd, name);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n", name, strerror(-fd));
		return AEE_EFAILED;
	}

	*out = fd;
	return 0;
}

static uint32_t apps_std_fwrite(void *data,
				const struct fastrpc_io_buffer *inbufs,
				struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const struct {
		uint32_t fd;
		uint32_t buf_size;
	} *first_in = inbufs[0].p;
	struct {
		uint32_t written;
		uint32_t is_eof;
	} *first_out = outbufs[0].p;
	ssize_t ret;

	ret = hexagonfs_write(ctx->fds, first_in->fd,
			      first_in->buf_size, inbufs[1].p);
	if (ret < 0) {
		fprintf(stderr, "Could not write file: %s\n", strerror(-ret));
		return AEE_EFAILED;
	}

	first_out->written = ret;
	first_out->is_eof = 0;

	return 0;
}

static uint32_t apps_std_fsync(void *data,
			       const struct fastrpc_io_buffer *inbufs,
			       struct fastrpc_io_buffer *outbufs)
{
	(void)data;
	(void)inbufs;
	(void)outbufs;
	return 0;
}

static uint32_t apps_std_fremove(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	const char *name = inbufs[1].p;
	char phys[PATH_MAX];

	(void)data;
	(void)outbufs;

	if (name[inbufs[1].s - 1] != 0)
		return AEE_EBADPARM;

	if (resolve_persist_registry_path(name, phys, sizeof(phys)) != 0) {
		fprintf(stderr, "Refused to remove %s\n", name);
		return AEE_EUNSUPPORTED;
	}

	if (unlink(phys) != 0 && errno != ENOENT) {
		fprintf(stderr, "Could not remove %s: %s\n", phys, strerror(errno));
		return AEE_EFAILED;
	}

	return 0;
}

static uint32_t apps_std_frename(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	const char *oldname = inbufs[1].p;
	const char *newname = inbufs[2].p;
	char old_phys[PATH_MAX], new_phys[PATH_MAX];

	(void)data;
	(void)outbufs;

	if (oldname[inbufs[1].s - 1] != 0 || newname[inbufs[2].s - 1] != 0)
		return AEE_EBADPARM;

	if (resolve_persist_registry_path(oldname, old_phys, sizeof(old_phys)) != 0 ||
	    resolve_persist_registry_path(newname, new_phys, sizeof(new_phys)) != 0) {
		fprintf(stderr, "Refused to rename %s -> %s\n", oldname, newname);
		return AEE_EUNSUPPORTED;
	}

	if (rename(old_phys, new_phys) != 0) {
		fprintf(stderr, "Could not rename %s -> %s: %s\n",
			old_phys, new_phys, strerror(errno));
		return AEE_EFAILED;
	}

	return 0;
}

static uint32_t apps_std_fopen_with_env(void *data,
					const struct fastrpc_io_buffer *inbufs,
					struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	uint32_t *out = outbufs[0].p;
	const char *mode;
	bool is_write = false;
	int flags, dirfd, fd;

	// The name and environment variable must also be NULL-terminated
	if (((const char *) inbufs[1].p)[inbufs[1].s - 1] != 0
	 || ((const char *) inbufs[3].p)[inbufs[3].s - 1] != 0
	 || ((const char *) inbufs[4].p)[inbufs[4].s - 1] != 0)
		return AEE_EBADPARM;

	mode = (const char *) inbufs[4].p;
	flags = mode_to_open_flags(mode, &is_write);
	if (is_write) {
		const char *name = (const char *) inbufs[3].p;
		char phys[PATH_MAX];

		if (resolve_persist_registry_path(name, phys, sizeof(phys)) != 0) {
			fprintf(stderr, "Tried to open %s for writing\n", name);
			return AEE_EUNSUPPORTED;
		}
		fd = hexagonfs_open_phys(ctx->fds, phys, flags, 0660);
		if (fd < 0) {
			fprintf(stderr, "Could not open %s for writing: %s\n",
				phys, strerror(-fd));
			return AEE_EFAILED;
		}
		*out = fd;
		return 0;
	}

	if (!strcmp(inbufs[1].p, "ADSP_LIBRARY_PATH")) {
		dirfd = ctx->adsp_library_dirfd;
	} else if (!strcmp(inbufs[1].p, "ADSP_AVS_CFG_PATH")) {
		dirfd = ctx->adsp_avs_cfg_dirfd;
	} else {
		fprintf(stderr, "Unknown search directory %s\n",
				(const char *) inbufs[1].p);
		return AEE_EBADPARM;
	}

	if (dirfd < 0) {
		fprintf(stderr, "Could not open virtual %s: %s\n",
				(const char *) inbufs[1].p, strerror(-dirfd));
		return AEE_EFAILED;
	}

	fd = hexagonfs_openat(ctx->fds, ctx->rootfd, dirfd, inbufs[3].p);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
				(const char *) inbufs[3].p,
				strerror(errno));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("openat($%s, %s, %c) -> %d\n", (const char *) inbufs[1].p,
					      (const char *) inbufs[3].p,
					      rw_mode,
					      fd);
#endif

	*out = fd;

	return 0;
}

static uint32_t apps_std_opendir(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	uint64_t *dir_out = outbufs[0].p;
	int ret;

	// The name must be NULL-terminated
	if (((const char *) inbufs[1].p)[inbufs[1].s - 1] != 0)
		return AEE_EBADPARM;

	ret = hexagonfs_openat(ctx->fds, ctx->rootfd, ctx->rootfd, inbufs[1].p);
	if (ret < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
				(const char *) inbufs[1].p,
				strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("opendir(%s) -> %d\n", (const char *) inbufs[1].p, ret);
#endif

	*dir_out = ret;

	return 0;
}

static uint32_t apps_std_closedir(void *data,
				  const struct fastrpc_io_buffer *inbufs,
				  struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const uint64_t *dir = inbufs[0].p;
	int ret;

	ret = hexagonfs_close(ctx->fds, *dir);
	if (ret)
		return AEE_EFAILED;

#ifdef HEXAGONRPC_VERBOSE
	printf("closedir(%ld)\n", *dir);
#endif

	return 0;
}

static uint32_t apps_std_readdir(void *data,
				 const struct fastrpc_io_buffer *inbufs,
				 struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	const uint64_t *dir = inbufs[0].p;
	struct {
		uint32_t inode;
		char name[255];
		uint32_t is_eof;
	} *first_out = outbufs[0].p;
	int ret;

	ret = hexagonfs_readdir(ctx->fds, *dir, 255, first_out->name);
	if (ret < 0) {
		fprintf(stderr, "Could not read from directory: %s\n",
				strerror(-ret));
		return AEE_EFAILED;
	}

#ifdef HEXAGONRPC_VERBOSE
	printf("readdir(%ld) -> %s\n", *dir, first_out->name);
#endif

	first_out->inode = 0;
	first_out->is_eof = (*first_out->name == '\0');

	return 0;
}

static uint32_t apps_std_stat(void *data,
			      const struct fastrpc_io_buffer *inbufs,
			      struct fastrpc_io_buffer *outbufs)
{
	struct apps_std_ctx *ctx = data;
	struct {
		uint64_t tsz; // Unknown purpose
		uint64_t dev;
		uint64_t ino;
		uint32_t mode;
		uint32_t nlink;
		uint64_t rdev;
		uint64_t size;
		int64_t atime;
		int64_t atimensec;
		int64_t mtime;
		int64_t mtimensec;
		int64_t ctime;
		int64_t ctimensec;
	} *first_out = outbufs[0].p;
	const char *pathname = inbufs[1].p;
	struct stat stats;
	int fd, ret;

	if (((const char *) inbufs[1].p)[inbufs[1].s - 1] != 0)
		return AEE_EBADPARM;

	fd = hexagonfs_openat(ctx->fds, ctx->rootfd, ctx->adsp_library_dirfd, pathname);
	if (fd < 0) {
		fprintf(stderr, "Could not open %s: %s\n",
				pathname, strerror(-fd));
		return AEE_EFAILED;
	}

	ret = hexagonfs_fstat(ctx->fds, fd, &stats);
	if (ret) {
		fprintf(stderr, "Could not stat %s: %s\n",
				pathname, strerror(-fd));
		return AEE_EFAILED;
	}

	hexagonfs_close(ctx->fds, fd);

#ifdef HEXAGONRPC_VERBOSE
	printf("stat(%s)\n", pathname);
#endif

	first_out->tsz = 0;

	first_out->dev = stats.st_dev;
	first_out->ino = stats.st_ino;
	first_out->mode = stats.st_mode;
	first_out->nlink = stats.st_nlink;
	first_out->rdev = stats.st_rdev;
	first_out->size = stats.st_size;
	first_out->atime = (int64_t) stats.st_atim.tv_sec;
	first_out->atimensec = (int64_t) stats.st_atim.tv_nsec;
	first_out->mtime = (int64_t) stats.st_mtim.tv_sec;
	first_out->mtimensec = (int64_t) stats.st_mtim.tv_nsec;
	first_out->ctime = (int64_t) stats.st_ctim.tv_nsec;
	first_out->ctimensec = (int64_t) stats.st_ctim.tv_nsec;

	return 0;
}

struct fastrpc_interface *fastrpc_apps_std_init(struct hexagonfs_dirent *root)
{
	struct fastrpc_interface *iface;
	struct apps_std_ctx *ctx;

	iface = malloc(sizeof(struct fastrpc_interface));
	if (iface == NULL)
		return NULL;

	ctx = calloc(1, sizeof(struct apps_std_ctx));
	if (ctx == NULL)
		goto err_free_iface;

	memcpy(iface, &apps_std_interface, sizeof(struct fastrpc_interface));

	ctx->rootfd = hexagonfs_open_root(ctx->fds, root);
	if (ctx->rootfd < 0)
		goto err_free_ctx;

	ctx->adsp_avs_cfg_dirfd = hexagonfs_openat(ctx->fds,
						   ctx->rootfd,
						   ctx->rootfd,
						   "/vendor/etc/acdbdata/");
	ctx->adsp_library_dirfd = hexagonfs_openat(ctx->fds,
						   ctx->rootfd,
						   ctx->rootfd,
						   "/usr/lib/qcom/adsp/");

	iface->data = ctx;

	return iface;

err_free_ctx:
	free(ctx);
err_free_iface:
	free(iface);

	return NULL;
}

void fastrpc_apps_std_deinit(struct fastrpc_interface *iface)
{
	struct apps_std_ctx *ctx = iface->data;
	int i;

	for (i = 0; i < HEXAGONFS_MAX_FD; i++) {
		if (ctx->fds[i] != NULL)
			hexagonfs_close(ctx->fds, i);
	}

	free(iface->data);
	free(iface);
}

static const struct fastrpc_function_impl apps_std_procs[] = {
	{
		.def = &apps_std_fopen_def,
		.impl = apps_std_fopen,
	},
	{ .def = NULL, .impl = NULL, },
	{
		.def = &apps_std_fflush_def,
		.impl = apps_std_fflush,
	},
	{
		.def = &apps_std_fclose_def,
		.impl = apps_std_fclose,
	},
	{
		.def = &apps_std_fread_def,
		.impl = apps_std_fread,
	},
	{
		.def = &apps_std_fwrite_def,
		.impl = apps_std_fwrite,
	},
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{
		.def = &apps_std_fseek_def,
		.impl = apps_std_fseek,
	},
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{
		.def = &apps_std_fopen_with_env_def,
		.impl = apps_std_fopen_with_env,
	},
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{
		.def = &apps_std_fsync_def,
		.impl = apps_std_fsync,
	},
	{
		.def = &apps_std_fremove_def,
		.impl = apps_std_fremove,
	},
	{ .def = NULL, .impl = NULL, },
	{
		.def = &apps_std_opendir_def,
		.impl = apps_std_opendir,
	},
	{
		.def = &apps_std_closedir_def,
		.impl = apps_std_closedir,
	},
	{
		.def = &apps_std_readdir_def,
		.impl = apps_std_readdir,
	},
	{ .def = NULL, .impl = NULL, },
	{ .def = NULL, .impl = NULL, },
	{
		.def = &apps_std_stat_def,
		.impl = apps_std_stat,
	},
	{ .def = NULL, .impl = NULL, },
	{
		.def = &apps_std_frename_def,
		.impl = apps_std_frename,
	},
};

const struct fastrpc_interface apps_std_interface = {
	.name = "apps_std",
	.n_procs = 34,
	.procs = apps_std_procs,
};
