/*
 * Copyright (C) 2012 Alexander Block.  All rights reserved.
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public
 * License v2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public
 * License along with this program; if not, write to the
 * Free Software Foundation, Inc., 59 Temple Place - Suite 330,
 * Boston, MA 021110-1307, USA.
 */

#include "kerncompat.h"
#include <unistd.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "kernel-shared/uapi/btrfs_tree.h"
#include "kernel-shared/uapi/btrfs.h"
#include "kernel-shared/send.h"
#include "crypto/crc32c.h"
#include "common/send-stream.h"
#include "common/messages.h"

struct btrfs_send_attribute {
	u16 tlv_type;
	/*
	 * Note: in btrfs_tlv_header, this is __le16, but we need 32 bits for
	 * attributes with file data as of version 2 of the send stream format.
	 */
	u32 tlv_len;
	char *data;
};

struct btrfs_send_stream {
	char *read_buf;
	size_t read_buf_size;
	int fd;

	int cmd;
	struct btrfs_send_attribute cmd_attrs[__BTRFS_SEND_A_MAX + 1];
	u32 version;

	/*
	 * end of last successful read, equivalent to start of current
	 * malformed part of block
	 */
	size_t stream_pos;

	struct btrfs_send_ops *ops;
	void *user;
} __attribute__((aligned(64)));

/*
 * Read len bytes to buf.
 * Return:
 *   0 - success
 * < 0 - negative errno in case of error
 * > 0 - no data read, EOF
 */
static int read_buf(struct btrfs_send_stream *sctx, char *buf, size_t len)
{
	int ret;
	size_t pos = 0;

	while (pos < len) {
		ssize_t rbytes;

		rbytes = read(sctx->fd, buf + pos, len - pos);
		if (rbytes < 0) {
			ret = -errno;
			error("read from stream failed: %m");
			goto out;
		}
		if (rbytes == 0) {
			ret = 1;
			goto out_eof;
		}
		pos += rbytes;
	}
	ret = 0;

out_eof:
	if (0 < pos && pos < len) {
		error("short read from stream: expected %zu read %zu", len, pos);
		ret = -EIO;
	} else {
		sctx->stream_pos += pos;
	}

out:
	return ret;
}

/*
 * Reads a single command from kernel space and decodes the TLV's into
 * sctx->cmd_attrs
 *
 * Returns:
 *   0 - success
 * < 0 - an error in the command
 */
static int read_cmd(struct btrfs_send_stream *sctx)
{
	int ret;
	u16 cmd;
	u32 cmd_len;
	char *data;
	u32 pos;
	u32 crc;
	u32 crc2;
	struct btrfs_cmd_header *cmd_hdr;
	size_t buf_len;

	memset(sctx->cmd_attrs, 0, sizeof(sctx->cmd_attrs));

	ret = read_buf(sctx, sctx->read_buf, sizeof(*cmd_hdr));
	if (ret < 0)
		goto out;
	if (ret) {
		ret = -EINVAL;
		error("unexpected EOF in stream");
		goto out;
	}

	/* The read_buf does not guarantee any alignment for any structures. */
	cmd_hdr = (struct btrfs_cmd_header *)sctx->read_buf;
	cmd_len = get_unaligned_le32(&cmd_hdr->len);
	cmd = get_unaligned_le16(&cmd_hdr->cmd);
	buf_len = sizeof(*cmd_hdr) + cmd_len;
	if (sctx->read_buf_size < buf_len) {
		void *new_read_buf;

		new_read_buf = realloc(sctx->read_buf, buf_len);
		if (!new_read_buf) {
			ret = -ENOMEM;
			errno = -ret;
			error_msg(ERROR_MSG_MEMORY, "read buffer for command");
			goto out;
		}
		sctx->read_buf = new_read_buf;
		sctx->read_buf_size = buf_len;
		/* We need to reset cmd_hdr after realloc of sctx->read_buf */
		cmd_hdr = (struct btrfs_cmd_header *)sctx->read_buf;
	}
	data = sctx->read_buf + sizeof(*cmd_hdr);
	ret = read_buf(sctx, data, cmd_len);
	if (ret < 0)
		goto out;
	if (ret) {
		ret = -EINVAL;
		error("unexpected EOF in stream");
		goto out;
	}

	crc = get_unaligned_le32(&cmd_hdr->crc);
	/* In send, CRC is computed with header crc = 0, replicate that */
	put_unaligned_le32(0, &cmd_hdr->crc);

	crc2 = crc32c(0, (unsigned char*)sctx->read_buf,
			sizeof(*cmd_hdr) + cmd_len);

	if (crc != crc2) {
		ret = -EINVAL;
		error("crc32 mismatch in command");
		goto out;
	}

	pos = 0;
	while (pos < cmd_len) {
		u16 tlv_type;
		struct btrfs_send_attribute *send_attr;

		if (cmd_len - pos < sizeof(__le16)) {
			error("send stream is truncated");
			ret = -EINVAL;
			goto out;
		}
		tlv_type = get_unaligned_le16(data);

		if (tlv_type == 0 || tlv_type > __BTRFS_SEND_A_MAX) {
			error("invalid tlv in cmd tlv_type = %hu", tlv_type);
			ret = -EINVAL;
			goto out;
		}

		send_attr = &sctx->cmd_attrs[tlv_type];
		send_attr->tlv_type = tlv_type;

		pos += sizeof(tlv_type);
		data += sizeof(tlv_type);
		if (sctx->version >= 2 && tlv_type == BTRFS_SEND_A_DATA) {
			send_attr->tlv_len = cmd_len - pos;
		} else {
			if (cmd_len - pos < sizeof(__le16)) {
				error("send stream is truncated");
				ret = -EINVAL;
				goto out;
			}
			send_attr->tlv_len = get_unaligned_le16(data);
			pos += sizeof(__le16);
			data += sizeof(__le16);
		}
		if (cmd_len - pos < send_attr->tlv_len) {
			error("send stream is truncated");
			ret = -EINVAL;
			goto out;
		}
		send_attr->data = data;
		pos += send_attr->tlv_len;
		data += send_attr->tlv_len;
	}

	sctx->cmd = cmd;
	ret = 0;

out:
	return ret;
}

static int tlv_get(struct btrfs_send_stream *sctx, int attr, void **data, int *len)
{
	int ret;
	struct btrfs_send_attribute *send_attr;

	if (attr <= 0 || attr > __BTRFS_SEND_A_MAX) {
		error("invalid attribute requested, attr = %d", attr);
		ret = -EINVAL;
		goto out;
	}

	send_attr = &sctx->cmd_attrs[attr];
	if (!send_attr->data) {
		error("attribute %d requested but not present", attr);
		ret = -ENOENT;
		goto out;
	}

	*len = send_attr->tlv_len;
	*data = send_attr->data;

	ret = 0;

out:
	return ret;
}

#define __TLV_GOTO_FAIL(expr) \
	if ((ret = expr) < 0) \
		goto tlv_get_failed;

#define __TLV_DO_WHILE_GOTO_FAIL(expr) \
	do { \
		__TLV_GOTO_FAIL(expr) \
	} while (0)


#define TLV_GET(s, attr, data, len) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get(s, attr, data, len))

#define TLV_CHECK_LEN(expected, got) \
	do { \
		if (expected != got) { \
			error("invalid size for attribute, " \
					"expected = %d, got = %d", \
					(int)expected, (int)got); \
			ret = -EINVAL; \
			goto tlv_get_failed; \
		} \
	} while (0)

#define TLV_GET_INT(s, attr, bits, v) \
	do { \
		__le##bits *__tmp; \
		int __len; \
		TLV_GET(s, attr, (void**)&__tmp, &__len); \
		TLV_CHECK_LEN(sizeof(*__tmp), __len); \
		*v = get_unaligned_le##bits(__tmp); \
	} while (0)

#define TLV_GET_U8(s, attr, v) TLV_GET_INT(s, attr, 8, v)
#define TLV_GET_U16(s, attr, v) TLV_GET_INT(s, attr, 16, v)
#define TLV_GET_U32(s, attr, v) TLV_GET_INT(s, attr, 32, v)
#define TLV_GET_U64(s, attr, v) TLV_GET_INT(s, attr, 64, v)

static int tlv_get_string(struct btrfs_send_stream *sctx, int attr, char **str)
{
	int ret;
	void *data;
	int len = 0;

	TLV_GET(sctx, attr, &data, &len);

	*str = malloc(len + 1);
	if (!*str)
		return -ENOMEM;

	memcpy(*str, data, len);
	(*str)[len] = 0;
	ret = 0;

tlv_get_failed:
	return ret;
}
#define TLV_GET_STRING(s, attr, str) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get_string(s, attr, str))

static int tlv_get_timespec(struct btrfs_send_stream *sctx,
			    int attr, struct timespec *ts)
{
	int ret;
	int len;
	struct btrfs_timespec *bts;

	TLV_GET(sctx, attr, (void**)&bts, &len);
	TLV_CHECK_LEN(sizeof(*bts), len);

	ts->tv_sec = get_unaligned_le64(&bts->sec);
	ts->tv_nsec = get_unaligned_le32(&bts->nsec);
	ret = 0;

tlv_get_failed:
	return ret;
}
#define TLV_GET_TIMESPEC(s, attr, ts) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get_timespec(s, attr, ts))

static int tlv_get_uuid(struct btrfs_send_stream *sctx, int attr, u8 *uuid)
{
	int ret;
	int len;
	void *data;

	TLV_GET(sctx, attr, &data, &len);
	TLV_CHECK_LEN(BTRFS_UUID_SIZE, len);
	memcpy(uuid, data, BTRFS_UUID_SIZE);

	ret = 0;

tlv_get_failed:
	return ret;
}
#define TLV_GET_UUID(s, attr, uuid) \
	__TLV_DO_WHILE_GOTO_FAIL(tlv_get_uuid(s, attr, uuid))

static int read_and_process_cmd(struct btrfs_send_stream *sctx)
{
	int ret;
	char *path = NULL;
	char *path_to = NULL;
	char *clone_path = NULL;
	char *xattr_name = NULL;
	void *xattr_data = NULL;
	void *data = NULL;
	u8 verity_algorithm;
	u32 verity_block_size;
	int verity_salt_len;
	void *verity_salt = NULL;
	int verity_sig_len;
	void *verity_sig = NULL;
	struct timespec at;
	struct timespec ct;
	struct timespec mt;
	u8 uuid[BTRFS_UUID_SIZE];
	u8 clone_uuid[BTRFS_UUID_SIZE];
	u32 compression;
	u32 encryption;
	u64 tmp;
	u64 tmp2;
	u64 ctransid;
	u64 clone_ctransid;
	u64 mode;
	u64 dev;
	u64 clone_offset;
	u64 offset;
	u64 ino;
	u64 unencoded_file_len;
	u64 unencoded_len;
	u64 unencoded_offset;
	u64 fileattr;
	int len;
	int xattr_len;
	int fallocate_mode;

	ret = read_cmd(sctx);
	if (ret)
		goto out;

	switch (sctx->cmd) {
	case BTRFS_SEND_C_SUBVOL:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_UUID(sctx, BTRFS_SEND_A_UUID, uuid);
		TLV_GET_U64(sctx, BTRFS_SEND_A_CTRANSID, &ctransid);
		ret = sctx->ops->subvol(path, uuid, ctransid, sctx->user);
		break;
	case BTRFS_SEND_C_SNAPSHOT:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_UUID(sctx, BTRFS_SEND_A_UUID, uuid);
		TLV_GET_U64(sctx, BTRFS_SEND_A_CTRANSID, &ctransid);
		TLV_GET_UUID(sctx, BTRFS_SEND_A_CLONE_UUID, clone_uuid);
		TLV_GET_U64(sctx, BTRFS_SEND_A_CLONE_CTRANSID, &clone_ctransid);
		ret = sctx->ops->snapshot(path, uuid, ctransid, clone_uuid,
				clone_ctransid, sctx->user);
		break;
	case BTRFS_SEND_C_MKFILE:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		/* ino is not passed to the callbacks in v1 */
		TLV_GET_U64(sctx, BTRFS_SEND_A_INO, &ino);
		ret = sctx->ops->mkfile(path, sctx->user);
		break;
	case BTRFS_SEND_C_MKDIR:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		/* ino is not passed to the callbacks in v1 */
		TLV_GET_U64(sctx, BTRFS_SEND_A_INO, &ino);
		ret = sctx->ops->mkdir(path, sctx->user);
		break;
	case BTRFS_SEND_C_MKNOD:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		/* ino is not passed to the callbacks in v1 */
		TLV_GET_U64(sctx, BTRFS_SEND_A_INO, &ino);
		TLV_GET_U64(sctx, BTRFS_SEND_A_MODE, &mode);
		TLV_GET_U64(sctx, BTRFS_SEND_A_RDEV, &dev);
		ret = sctx->ops->mknod(path, mode, dev, sctx->user);
		break;
	case BTRFS_SEND_C_MKFIFO:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		/* ino is not passed to the callbacks in v1 */
		TLV_GET_U64(sctx, BTRFS_SEND_A_INO, &ino);
		ret = sctx->ops->mkfifo(path, sctx->user);
		break;
	case BTRFS_SEND_C_MKSOCK:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		/* ino is not passed to the callbacks in v1 */
		TLV_GET_U64(sctx, BTRFS_SEND_A_INO, &ino);
		ret = sctx->ops->mksock(path, sctx->user);
		break;
	case BTRFS_SEND_C_SYMLINK:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		/* ino is not passed to the callbacks in v1 */
		TLV_GET_U64(sctx, BTRFS_SEND_A_INO, &ino);
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH_LINK, &path_to);
		ret = sctx->ops->symlink(path, path_to, sctx->user);
		break;
	case BTRFS_SEND_C_RENAME:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH_TO, &path_to);
		ret = sctx->ops->rename(path, path_to, sctx->user);
		break;
	case BTRFS_SEND_C_LINK:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH_LINK, &path_to);
		ret = sctx->ops->link(path, path_to, sctx->user);
		break;
	case BTRFS_SEND_C_UNLINK:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		ret = sctx->ops->unlink(path, sctx->user);
		break;
	case BTRFS_SEND_C_RMDIR:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		ret = sctx->ops->rmdir(path, sctx->user);
		break;
	case BTRFS_SEND_C_WRITE:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET(sctx, BTRFS_SEND_A_DATA, &data, &len);
		ret = sctx->ops->write(path, data, offset, len, sctx->user);
		break;
	case BTRFS_SEND_C_ENCODED_WRITE:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET_U64(sctx, BTRFS_SEND_A_UNENCODED_FILE_LEN,
			    &unencoded_file_len);
		TLV_GET_U64(sctx, BTRFS_SEND_A_UNENCODED_LEN, &unencoded_len);
		TLV_GET_U64(sctx, BTRFS_SEND_A_UNENCODED_OFFSET,
			    &unencoded_offset);
		/* Compression and encryption default to none if omitted. */
		if (sctx->cmd_attrs[BTRFS_SEND_A_COMPRESSION].data)
			TLV_GET_U32(sctx, BTRFS_SEND_A_COMPRESSION, &compression);
		else
			compression = BTRFS_ENCODED_IO_COMPRESSION_NONE;
		if (sctx->cmd_attrs[BTRFS_SEND_A_ENCRYPTION].data)
			TLV_GET_U32(sctx, BTRFS_SEND_A_ENCRYPTION, &encryption);
		else
			encryption = BTRFS_ENCODED_IO_ENCRYPTION_NONE;
		TLV_GET(sctx, BTRFS_SEND_A_DATA, &data, &len);
		ret = sctx->ops->encoded_write(path, data, offset, len,
					       unencoded_file_len,
					       unencoded_len, unencoded_offset,
					       compression, encryption,
					       sctx->user);
		break;
	case BTRFS_SEND_C_CLONE:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET_U64(sctx, BTRFS_SEND_A_CLONE_LEN, &len);
		TLV_GET_UUID(sctx, BTRFS_SEND_A_CLONE_UUID, clone_uuid);
		TLV_GET_U64(sctx, BTRFS_SEND_A_CLONE_CTRANSID, &clone_ctransid);
		TLV_GET_STRING(sctx, BTRFS_SEND_A_CLONE_PATH, &clone_path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_CLONE_OFFSET, &clone_offset);
		ret = sctx->ops->clone(path, offset, len, clone_uuid,
				clone_ctransid, clone_path, clone_offset,
				sctx->user);
		break;
	case BTRFS_SEND_C_SET_XATTR:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(sctx, BTRFS_SEND_A_XATTR_NAME, &xattr_name);
		TLV_GET(sctx, BTRFS_SEND_A_XATTR_DATA, &xattr_data, &xattr_len);
		ret = sctx->ops->set_xattr(path, xattr_name, xattr_data,
				xattr_len, sctx->user);
		break;
	case BTRFS_SEND_C_REMOVE_XATTR:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_STRING(sctx, BTRFS_SEND_A_XATTR_NAME, &xattr_name);
		ret = sctx->ops->remove_xattr(path, xattr_name, sctx->user);
		break;
	case BTRFS_SEND_C_TRUNCATE:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_SIZE, &tmp);
		ret = sctx->ops->truncate(path, tmp, sctx->user);
		break;
	case BTRFS_SEND_C_CHMOD:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_MODE, &tmp);
		ret = sctx->ops->chmod(path, tmp, sctx->user);
		break;
	case BTRFS_SEND_C_CHOWN:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_UID, &tmp);
		TLV_GET_U64(sctx, BTRFS_SEND_A_GID, &tmp2);
		ret = sctx->ops->chown(path, tmp, tmp2, sctx->user);
		break;
	case BTRFS_SEND_C_UTIMES:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_TIMESPEC(sctx, BTRFS_SEND_A_ATIME, &at);
		TLV_GET_TIMESPEC(sctx, BTRFS_SEND_A_MTIME, &mt);
		TLV_GET_TIMESPEC(sctx, BTRFS_SEND_A_CTIME, &ct);
		ret = sctx->ops->utimes(path, &at, &mt, &ct, sctx->user);
		break;
	case BTRFS_SEND_C_UPDATE_EXTENT:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET_U64(sctx, BTRFS_SEND_A_SIZE, &tmp);
		ret = sctx->ops->update_extent(path, offset, tmp, sctx->user);
		break;
	case BTRFS_SEND_C_ENABLE_VERITY:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U8(sctx, BTRFS_SEND_A_VERITY_ALGORITHM, &verity_algorithm);
		TLV_GET_U32(sctx, BTRFS_SEND_A_VERITY_BLOCK_SIZE, &verity_block_size);
		TLV_GET(sctx, BTRFS_SEND_A_VERITY_SALT_DATA, &verity_salt, &verity_salt_len);
		TLV_GET(sctx, BTRFS_SEND_A_VERITY_SIG_DATA, &verity_sig, &verity_sig_len);
		ret = sctx->ops->enable_verity(path, verity_algorithm, verity_block_size,
					       verity_salt_len, verity_salt,
					       verity_sig_len, verity_sig, sctx->user);
		break;
	case BTRFS_SEND_C_END:
		ret = 1;
		break;
	case BTRFS_SEND_C_FALLOCATE:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U32(sctx, BTRFS_SEND_A_FALLOCATE_MODE, &fallocate_mode);
		TLV_GET_U64(sctx, BTRFS_SEND_A_FILE_OFFSET, &offset);
		TLV_GET_U64(sctx, BTRFS_SEND_A_SIZE, &tmp);
		ret = sctx->ops->fallocate(path, fallocate_mode, offset, tmp,
					   sctx->user);
		break;
	case BTRFS_SEND_C_FILEATTR:
		TLV_GET_STRING(sctx, BTRFS_SEND_A_PATH, &path);
		TLV_GET_U64(sctx, BTRFS_SEND_A_FILEATTR, &fileattr);
		ret = sctx->ops->fileattr(path, fileattr, sctx->user);
		break;
	}


tlv_get_failed:
out:
	free(path);
	free(path_to);
	free(clone_path);
	free(xattr_name);
	return ret;
}

/*
 * If max_errors is 0, then don't stop processing the stream if one of the
 * callbacks in btrfs_send_ops structure returns an error. If greater than
 * zero, stop after max_errors errors happened.
 */
int btrfs_read_and_process_send_stream(int fd,
				       struct btrfs_send_ops *ops, void *user,
				       int honor_end_cmd,
				       u64 max_errors)
{
	int ret;
	struct btrfs_send_stream sctx;
	struct btrfs_stream_header hdr;
	u64 errors = 0;
	int last_err = 0;

	sctx.fd = fd;
	sctx.ops = ops;
	sctx.user = user;
	sctx.stream_pos = 0;

	ret = read_buf(&sctx, (char*)&hdr, sizeof(hdr));
	if (ret < 0)
		goto out;
	if (ret) {
		ret = -ENODATA;
		goto out;
	}

	if (strcmp(hdr.magic, BTRFS_SEND_STREAM_MAGIC)) {
		ret = -EINVAL;
		error("unexpected header");
		goto out;
	}

	sctx.version = le32_to_cpu(hdr.version);
	if (sctx.version > BTRFS_SEND_STREAM_VERSION) {
		ret = -EINVAL;
		error("stream version %d not supported, please use newer version",
				sctx.version);
		goto out;
	}

	sctx.read_buf = malloc(BTRFS_SEND_BUF_SIZE_V1);
	if (!sctx.read_buf) {
		ret = -ENOMEM;
		error_msg(ERROR_MSG_MEMORY, "send stream read buffer");
		goto out;
	}
	sctx.read_buf_size = BTRFS_SEND_BUF_SIZE_V1;

	while (1) {
		ret = read_and_process_cmd(&sctx);
		if (ret < 0) {
			last_err = ret;
			errors++;
			if (max_errors > 0 && errors >= max_errors)
				break;
		} else if (ret > 0) {
			if (!honor_end_cmd)
				ret = 0;
			break;
		}
	}
	free(sctx.read_buf);

out:
	if (last_err && !ret)
		ret = last_err;

	return ret;
}
