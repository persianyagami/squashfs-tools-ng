/* SPDX-License-Identifier: GPL-3.0-or-later */
/*
 * io_stdin.c
 *
 * Copyright (C) 2019 David Oberhollenzer <goliath@infraroot.at>
 */
#include "common.h"

#include <stdlib.h>
#include <string.h>
#include <errno.h>


typedef struct {
	sqfs_file_t base;

	const sparse_map_t *map;
	sqfs_u64 offset;
	sqfs_u64 real_size;
	sqfs_u64 apparent_size;
	FILE *fp;
} sqfs_file_stdinout_t;


static void stdinout_destroy(sqfs_object_t *base)
{
	free(base);
}

static sqfs_u64 stdinout_get_size(const sqfs_file_t *base)
{
	return ((const sqfs_file_stdinout_t *)base)->apparent_size;
}

static int stdinout_truncate(sqfs_file_t *base, sqfs_u64 size)
{
	(void)base; (void)size;
	return SQFS_ERROR_IO;
}

static int stdin_read_at(sqfs_file_t *base, sqfs_u64 offset,
			 void *buffer, size_t size)
{
	sqfs_file_stdinout_t *file = (sqfs_file_stdinout_t *)base;
	size_t temp_size = 0;
	sqfs_u8 *temp = NULL;
	sqfs_u64 diff;
	size_t ret;

	if (offset < file->offset)
		return SQFS_ERROR_IO;

	if (offset > file->offset) {
		temp_size = 1024;
		temp = alloca(temp_size);
	}

	if (offset >= file->real_size || (offset + size) > file->real_size)
		return SQFS_ERROR_OUT_OF_BOUNDS;

	while (size > 0) {
		if (ferror(file->fp))
			return SQFS_ERROR_IO;

		if (feof(file->fp))
			return SQFS_ERROR_OUT_OF_BOUNDS;

		if (offset > file->offset) {
			diff = file->offset - offset;
			diff = diff > (sqfs_u64)temp_size ? temp_size : diff;

			ret = fread(temp, 1, diff, file->fp);
		} else {
			ret = fread(buffer, 1, size, file->fp);
		}

		if (offset <= file->offset) {
			buffer = (char *)buffer + ret;
			size -= ret;
			offset += ret;
		}

		file->offset += ret;
	}

	return 0;
}

static int stdin_read_condensed(sqfs_file_t *base, sqfs_u64 offset,
				void *buffer, size_t size)
{
	sqfs_file_stdinout_t *file = (sqfs_file_stdinout_t *)base;
	sqfs_u64 poffset = 0, src_start;
	size_t dst_start, diff, count;
	const sparse_map_t *it;
	int err;

	memset(buffer, 0, size);

	for (it = file->map; it != NULL; it = it->next) {
		if (it->offset + it->count <= offset) {
			poffset += it->count;
			continue;
		}

		if (it->offset >= offset + size) {
			poffset += it->count;
			continue;
		}

		count = size;

		if (offset + count >= it->offset + it->count)
			count = it->offset + it->count - offset;

		if (it->offset < offset) {
			diff = offset - it->offset;

			src_start = poffset + diff;
			dst_start = 0;
			count -= diff;
		} else if (it->offset > offset) {
			diff = it->offset - offset;

			src_start = poffset;
			dst_start = diff;
		} else {
			src_start = poffset;
			dst_start = 0;
		}

		err = stdin_read_at(base, src_start,
				    (char *)buffer + dst_start, count);
		if (err)
			return err;

		poffset += it->count;
	}

	return 0;
}

static int stdin_write_at(sqfs_file_t *base, sqfs_u64 offset,
			  const void *buffer, size_t size)
{
	(void)base; (void)offset; (void)buffer; (void)size;
	return SQFS_ERROR_IO;
}

sqfs_file_t *sqfs_get_stdin_file(FILE *fp, const sparse_map_t *map,
				 sqfs_u64 size)
{
	sqfs_file_stdinout_t *file = calloc(1, sizeof(*file));
	sqfs_file_t *base = (sqfs_file_t *)file;
	const sparse_map_t *it;

	if (file == NULL)
		return NULL;

	file->apparent_size = size;
	file->map = map;
	file->fp = fp;

	if (map != NULL) {
		for (it = map; it != NULL; it = it->next)
			file->real_size += map->count;
	} else {
		file->real_size = size;
	}

	((sqfs_object_t *)base)->destroy = stdinout_destroy;
	base->write_at = stdin_write_at;
	base->get_size = stdinout_get_size;
	base->truncate = stdinout_truncate;

	if (map == NULL) {
		base->read_at = stdin_read_at;
	} else {
		base->read_at = stdin_read_condensed;
	}
	return base;
}
