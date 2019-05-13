/* SPDX-License-Identifier: GPL-3.0-or-later */
#include "meta_writer.h"
#include "mkfs.h"
#include "util.h"

#include <assert.h>

static const struct {
	const char *prefix;
	E_SQFS_XATTR_TYPE type;
} xattr_types[] = {
	{ "user.", SQUASHFS_XATTR_USER },
	{ "trusted.", SQUASHFS_XATTR_TRUSTED },
	{ "security.", SQUASHFS_XATTR_SECURITY },
};

static int get_prefix(const char *key)
{
	size_t i, len;

	for (i = 0; i < sizeof(xattr_types) / sizeof(xattr_types[0]); ++i) {
		len = strlen(xattr_types[i].prefix);

		if (strncmp(key, xattr_types[i].prefix, len) == 0 &&
		    strlen(key) > len) {
			return xattr_types[i].type;
		}
	}

	fprintf(stderr, "unknown xattr key '%s'\n", key);
	return -1;
}

static int write_pair(meta_writer_t *mw, const char *key, const char *value)
{
	sqfs_xattr_entry_t kent;
	sqfs_xattr_value_t vent;
	int type;

	type = get_prefix(key);
	if (type < 0)
		return -1;

	key = strchr(key, '.');
	assert(key != NULL);
	++key;

	kent.type = htole16(type);
	kent.size = htole16(strlen(key));

	if (meta_writer_append(mw, &kent, sizeof(kent)))
		return -1;
	if (meta_writer_append(mw, key, strlen(key)))
		return -1;

	vent.size = htole32(strlen(value));

	if (meta_writer_append(mw, &vent, sizeof(vent)))
		return -1;
	if (meta_writer_append(mw, value, strlen(value)))
		return -1;

	return 0;
}

static int write_kv_pairs(sqfs_info_t *info, meta_writer_t *mw,
			  tree_xattr_t *xattr)
{
	uint32_t key_idx, val_idx;
	const char *key, *value;
	size_t i;

	for (i = 0; i < xattr->num_attr; ++i) {
		key_idx = (xattr->ref[i] >> 32) & 0xFFFFFFFF;
		val_idx = xattr->ref[i] & 0xFFFFFFFF;

		key = str_table_get_string(&info->fs.xattr_keys, key_idx);
		value = str_table_get_string(&info->fs.xattr_values, val_idx);

		if (write_pair(mw, key, value))
			return -1;

		xattr->size += sizeof(sqfs_xattr_entry_t) + strlen(key);
		xattr->size += sizeof(sqfs_xattr_value_t) + strlen(value);
	}

	return 0;
}

int write_xattr(sqfs_info_t *info)
{
	uint64_t kv_start, id_start, *tbl;
	size_t i = 0, count = 0, blocks;
	sqfs_xattr_id_table_t idtbl;
	sqfs_xattr_id_t id_ent;
	meta_writer_t *mw;
	tree_xattr_t *it;
	ssize_t ret;

	if (info->fs.xattr == NULL)
		return 0;

	mw = meta_writer_create(info->outfd, info->cmp);
	if (mw == NULL)
		return -1;

	/* write xattr key-value pairs */
	kv_start = info->super.bytes_used;

	for (it = info->fs.xattr; it != NULL; it = it->next) {
		it->block = mw->block_offset;
		it->offset = mw->offset;
		it->size = 0;

		if (write_kv_pairs(info, mw, it))
			goto fail_mw;

		++count;
	}

	if (meta_writer_flush(mw))
		goto fail_mw;

	info->super.bytes_used += mw->block_offset;
	mw->block_offset = 0;

	/* allocate location table */
	blocks = (count * sizeof(id_ent)) / SQFS_META_BLOCK_SIZE;

	if ((count * sizeof(id_ent)) % SQFS_META_BLOCK_SIZE)
		++blocks;

	tbl = calloc(sizeof(uint64_t), blocks);

	if (tbl == NULL) {
		perror("generating xattr ID table");
		goto fail_mw;
	}

	/* write ID table referring to key value pairs, record offsets */
	id_start = mw->block_offset;
	tbl[i++] = htole64(info->super.bytes_used + id_start);

	for (it = info->fs.xattr; it != NULL; it = it->next) {
		id_ent.xattr = htole64((it->block << 16) | it->offset);
		id_ent.count = htole32(it->num_attr);
		id_ent.size = htole32(it->size);

		if (meta_writer_append(mw, &id_ent, sizeof(id_ent)))
			goto fail_tbl;

		if (mw->block_offset != id_start) {
			id_start = mw->block_offset;
			tbl[i++] = htole64(info->super.bytes_used + id_start);
		}
	}

	if (meta_writer_flush(mw))
		goto fail_tbl;

	info->super.bytes_used += mw->block_offset;
	mw->block_offset = 0;

	/* write offset table */
	idtbl.xattr_table_start = htole64(kv_start);
	idtbl.xattr_ids = htole32(count);
	idtbl.unused = 0;

	ret = write_retry(info->outfd, &idtbl, sizeof(idtbl));
	if (ret < 0)
		goto fail_wr;
	if ((size_t)ret < sizeof(idtbl))
		goto fail_trunc;

	write_retry(info->outfd, tbl, sizeof(tbl[0]) * blocks);
	if (ret < 0)
		goto fail_wr;
	if ((size_t)ret < sizeof(tbl[0]) * blocks)
		goto fail_trunc;

	info->super.xattr_id_table_start = info->super.bytes_used;
	info->super.bytes_used += sizeof(idtbl) + sizeof(tbl[0]) * blocks;

	free(tbl);
	meta_writer_destroy(mw);
	return 0;
fail_wr:
	perror("writing xattr ID table");
	goto fail_tbl;
fail_trunc:
	fputs("writing xattr ID table: truncated write\n", stderr);
	goto fail_tbl;
fail_tbl:
	free(tbl);
fail_mw:
	meta_writer_destroy(mw);
	return -1;
}
