/*
 * Copyright (c) 2016-2022, Linaro Limited
 * Copyright (C) 2020-2023 Xiaomi Corporation
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_ext.h>
#include <sys/queue.h>
#include <tee/tee_fs.h>
#include <tee/tee_fs_rpc.h>
#include <tee/tee_fs_key_manager.h>
#include <trace.h>
#include <utee_defines.h>
#include <util.h>
#include <tee/error_messages.h>
#include <kernel/tee_ta_manager.h>

/*
 * This file implements the tee_file_operations structure for a secure
 * filesystem based on single file in normal world.
 *
 * All fields in the REE file are duplicated with two versions 0 and 1. The
 * active meta-data block is selected by the lowest bit in the
 * meta-counter.  The active file block is selected by corresponding bit
 * number in struct tee_fs_file_info.backup_version_table.
 *
 * The atomicity of each operation is ensured by updating meta-counter when
 * everything in the secondary blocks (both meta-data and file-data blocks)
 * are successfully written.  The main purpose of the code below is to
 * perform block encryption and authentication of the file data, and
 * properly handle seeking through the file. One file (in the sense of
 * struct tee_file_operations) maps to one file in the REE filesystem, and
 * has the following structure:
 *
 * [ 4 bytes meta-counter]
 * [ meta-data version 0][ meta-data version 1 ]
 * [ Block 0 version 0 ][ Block 0 version 1 ]
 * [ Block 1 version 0 ][ Block 1 version 1 ]
 * ...
 * [ Block n version 0 ][ Block n version 1 ]
 *
 * One meta-data block is built up as:
 * [ struct meta_header | struct tee_fs_get_header_size ]
 *
 * One data block is built up as:
 * [ struct block_header | BLOCK_FILE_SIZE bytes ]
 *
 * struct meta_header and struct block_header are defined in
 * tee_fs_key_manager.h.
 *
 */
extern void dump_buf(char *title, void *buf, uint32_t size);

#define BLOCK_SHIFT	8

#define BLOCK_SIZE	(1 << BLOCK_SHIFT)

#define MAX_FILE_SIZE	(BLOCK_SIZE * NUM_BLOCKS_PER_FILE)

struct tee_fs_fd {
	uint32_t meta_counter;
	struct tee_fs_file_meta meta;
	tee_fs_off_t pos;
	uint32_t flags;
	bool is_new_file;
	int fd;
};

static inline int pos_to_block_num(int position)
{
	return position >> BLOCK_SHIFT;
}

static inline int get_last_block_num(size_t size)
{
	return pos_to_block_num(size - 1);
}

static bool get_backup_version_of_block(struct tee_fs_file_meta *meta,
					size_t block_num)
{
	uint32_t index = (block_num / 32);
	uint32_t block_mask = 1 << (block_num % 32);

	return !!(meta->info.backup_version_table[index] & block_mask);
}

static inline void toggle_backup_version_of_block(
		struct tee_fs_file_meta *meta,
		size_t block_num)
{
	uint32_t index = (block_num / 32);
	uint32_t block_mask = 1 << (block_num % 32);

	meta->info.backup_version_table[index] ^= block_mask;
}

static size_t meta_size(void)
{
	return tee_fs_get_header_size(META_FILE) +
	       sizeof(struct tee_fs_file_meta);
}

static size_t meta_pos_raw(struct tee_fs_fd *fdp, bool active)
{
	size_t offs = sizeof(uint32_t);

	if ((fdp->meta_counter & 1) == active)
		offs += meta_size();
	return offs;
}

static size_t block_size_raw(void)
{
	return tee_fs_get_header_size(BLOCK_FILE) + BLOCK_SIZE;
}

static size_t block_pos_raw(struct tee_fs_file_meta *meta, size_t block_num,
			    bool active)
{
	size_t n = block_num * 2;

	if (active == get_backup_version_of_block(meta, block_num))
		n++;

	return sizeof(uint32_t) + meta_size() * 2 + n * block_size_raw();
}

/*
 * encrypted_fek: as input for META_FILE and BLOCK_FILE
 */
static TEE_Result encrypt_and_write_file(struct tee_fs_fd *fdp,
		enum tee_fs_file_type file_type, size_t offs,
		void *data_in, size_t data_in_size,
		uint8_t *encrypted_fek)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	void *ciphertext = NULL;
	size_t header_size = tee_fs_get_header_size(file_type);
	size_t ciphertext_size = header_size + data_in_size;

	ciphertext = malloc(ciphertext_size);
	if (!ciphertext) {
		EMSG(ERR_MSG_OUT_OF_MEMORY ": %zu\n", ciphertext_size);
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	res = tee_fs_encrypt_file(file_type, data_in, data_in_size,
				  ciphertext, &ciphertext_size, encrypted_fek);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		goto exit;
	}

	res = tee_fs_rpc_write(fdp->fd, ciphertext, &ciphertext_size, offs);
exit:
	free(ciphertext);
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

/*
 * encrypted_fek: as output for META_FILE
 *                as input for BLOCK_FILE
 */
static TEE_Result read_and_decrypt_file(struct tee_fs_fd *fdp,
		enum tee_fs_file_type file_type, size_t offs,
		void *data_out, size_t *data_out_size,
		uint8_t *encrypted_fek)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	size_t bytes = 0;
	void *ciphertext = NULL;

	bytes = *data_out_size + tee_fs_get_header_size(file_type);
	ciphertext = malloc(bytes);
	if (!ciphertext) {
		EMSG(ERR_MSG_OUT_OF_MEMORY ": %zu\n", bytes);
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	res = tee_fs_rpc_read(fdp->fd, ciphertext, &bytes, offs);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		goto exit;
	}

	if (!bytes) {
		*data_out_size = 0;
		res = TEE_SUCCESS;
		goto exit;
	}

	res = tee_fs_decrypt_file(file_type, ciphertext, bytes, data_out,
				  data_out_size, encrypted_fek);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_CORRUPT_OBJECT "\n");
		res = TEE_ERROR_CORRUPT_OBJECT;
	}
exit:
	free(ciphertext);
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result write_meta_file(struct tee_fs_fd *fdp,
		struct tee_fs_file_meta *meta)
{
	size_t offs = meta_pos_raw(fdp, false);

	DMSG("meta file --active: %d, --offs: %zd\n", (unsigned int)false, offs);
	return encrypt_and_write_file(fdp, META_FILE, offs,
			(void *)&meta->info, sizeof(meta->info),
			meta->encrypted_fek);
}

static TEE_Result write_meta_counter(struct tee_fs_fd *fdp)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	size_t bytes = sizeof(uint32_t);
	uint8_t data[sizeof(uint32_t)];
	memcpy(data, &fdp->meta_counter, bytes);

	res = tee_fs_rpc_write(fdp->fd, (void *)data, &bytes, 0);

	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result create_meta(struct tee_fs_fd *fdp, const char *fname)
{
	TEE_Result res;

	memset(fdp->meta.info.backup_version_table, 0xff,
		sizeof(fdp->meta.info.backup_version_table));
	fdp->meta.info.length = 0;

	struct ts_session *ts_sess = ts_get_current_session();
	res = tee_fs_generate_fek(&ts_sess->ctx->uuid,
			fdp->meta.encrypted_fek, TEE_FS_KM_FEK_SIZE);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		return res;
	}
#ifdef DEBUG_KEY_MANAGER
	dump_buf("WARNING: meta.encrypted_fek", fdp->meta.encrypted_fek, TEE_FS_KM_FEK_SIZE);
#endif

	res = tee_fs_rpc_open(fname, true, &fdp->fd);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": %s, 0x%08lx\n", fname, res);
		return res;
	}
	fdp->meta.counter = fdp->meta_counter;

	res = write_meta_file(fdp, &fdp->meta);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		return res;
	}
	return write_meta_counter(fdp);
}

static TEE_Result commit_meta_file(struct tee_fs_fd *fdp,
				   struct tee_fs_file_meta *new_meta)
{
	TEE_Result res;

	new_meta->counter = fdp->meta_counter + 1;

	DMSG("new meta counter: 0x%08lx\n", new_meta->counter);
	res = write_meta_file(fdp, new_meta);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		return res;
	}

	/*
	 * From now on the new meta is successfully committed,
	 * change tee_fs_fd accordingly
	 */
	fdp->meta = *new_meta;
	fdp->meta_counter = fdp->meta.counter;

	return write_meta_counter(fdp);
}

static TEE_Result read_meta_file(struct tee_fs_fd *fdp,
		struct tee_fs_file_meta *meta)
{
	size_t meta_info_size = sizeof(struct tee_fs_file_info);
	size_t offs = meta_pos_raw(fdp, true);

	DMSG("meta file --active: %ld, --offs: %zd\n", (uint32_t)true, offs);
	return read_and_decrypt_file(fdp, META_FILE, offs,
				     &meta->info, &meta_info_size,
				     meta->encrypted_fek);
}

static TEE_Result read_meta_counter(struct tee_fs_fd *fdp)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	uint8_t data[sizeof(uint32_t)];
	size_t bytes = sizeof(uint32_t);

	res = tee_fs_rpc_read(fdp->fd, (void *)data, &bytes, 0);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": %d, 0x%08lx\n", fdp->fd, res);
		goto exit;
	}

	if (bytes != sizeof(uint32_t)) {
		EMSG(ERR_MSG_CORRUPT_OBJECT ": %zu\n", bytes);
		res = TEE_ERROR_CORRUPT_OBJECT;
		goto exit;
	}
	memcpy(&fdp->meta_counter, data, bytes);

exit:
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result read_meta(struct tee_fs_fd *fdp, const char *fname)
{
	TEE_Result res;
	res = tee_fs_rpc_open(fname, false, &fdp->fd);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": %s, 0x%08lx\n", fname, res);
		return res;
	}

	res = read_meta_counter(fdp);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		return res;
	}

	return read_meta_file(fdp, &fdp->meta);
}

static TEE_Result read_block(struct tee_fs_fd *fdp, int bnum, uint8_t *data)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	size_t ct_size = block_size_raw();
	size_t out_size = BLOCK_SIZE;
	ssize_t pos = block_pos_raw(&fdp->meta, bnum, true);
	void *ct = NULL;
	ct = malloc(ct_size);
	if (!ct) {
		EMSG(ERR_MSG_OUT_OF_MEMORY ": %zu\n", ct_size);
		return TEE_ERROR_OUT_OF_MEMORY;
	}

	DMSG("read data block from file\n");
	res = tee_fs_rpc_read(fdp->fd, ct, &ct_size, pos);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		goto exit;
	}

	if (!ct_size) {
		memset(data, 0, BLOCK_SIZE);
		res = TEE_SUCCESS; /* Block does not exist */
		goto exit;
	}
	DMSG("data block size: %zd\n", ct_size);
	DMSG("decrypt data block\n");
	res = tee_fs_decrypt_file(BLOCK_FILE, ct, ct_size, data,
				   &out_size, fdp->meta.encrypted_fek);
exit:
	free(ct);
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result write_block(struct tee_fs_fd *fdp, size_t bnum, uint8_t *data,
			      struct tee_fs_file_meta *new_meta)
{
	TEE_Result res;
	size_t offs = block_pos_raw(new_meta, bnum, false);

	res = encrypt_and_write_file(fdp, BLOCK_FILE, offs, data,
				     BLOCK_SIZE, new_meta->encrypted_fek);
	if (res == TEE_SUCCESS)
		toggle_backup_version_of_block(new_meta, bnum);
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result out_of_place_write(struct tee_fs_fd *fdp, const void *buf,
		size_t len, struct tee_fs_file_meta *new_meta)
{
	TEE_Result res = TEE_ERROR_GENERIC;
	int start_block_num = pos_to_block_num(fdp->pos);
	int end_block_num = pos_to_block_num(fdp->pos + len - 1);
	size_t remain_bytes = len;
	uint8_t *data_ptr = (uint8_t *)buf;
	uint8_t block[BLOCK_SIZE];
	int orig_pos = fdp->pos;

	DMSG("start_block_num: %d, end_block_num: %d\n", start_block_num, end_block_num);
	while (start_block_num <= end_block_num) {
		int offset = fdp->pos % BLOCK_SIZE;
		size_t size_to_write = MIN(remain_bytes, (size_t)BLOCK_SIZE);

		if (size_to_write + offset > BLOCK_SIZE)
			size_to_write = BLOCK_SIZE - offset;

		res = read_block(fdp, start_block_num, block);
		if (res == TEE_ERROR_ITEM_NOT_FOUND)
			memset(block, 0, BLOCK_SIZE);
		else if (res != TEE_SUCCESS) {
			EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
			goto exit;
		}

		if (data_ptr)
			memcpy(block + offset, data_ptr, size_to_write);
		else
			memset(block + offset, 0, size_to_write);

		res = write_block(fdp, start_block_num, block, new_meta);
		if (res != TEE_SUCCESS) {
			EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
			goto exit;
		}

		if (data_ptr)
			data_ptr += size_to_write;
		remain_bytes -= size_to_write;
		start_block_num++;
		fdp->pos += size_to_write;
	}

	if (fdp->pos > (tee_fs_off_t)new_meta->info.length)
		new_meta->info.length = fdp->pos;
	DMSG("updated meta.info.length: %ld\n", (uint32_t)fdp->pos);
exit:
	if (res != TEE_SUCCESS)
		fdp->pos = orig_pos;
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result open_internal(const char *file, bool create,
				struct tee_file_handle **fh)
{
	TEE_Result res;
	size_t len;
	struct tee_fs_fd *fdp = NULL;

	if (!file) {
		EMSG(ERR_MSG_BAD_PARAMETERS "\n");
		return TEE_ERROR_BAD_PARAMETERS;
	}

	len = strlen(file) + 1;
	if (len > TEE_FS_NAME_MAX) {
		EMSG(ERR_MSG_BAD_PARAMETERS ": %zu\n", len);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	fdp = calloc(1, sizeof(struct tee_fs_fd));
	if (!fdp) {
		EMSG(ERR_MSG_OUT_OF_MEMORY ": %d\n", sizeof(struct tee_fs_fd));
		return TEE_ERROR_OUT_OF_MEMORY;
	}
	fdp->fd = -1;

	if (create)
		res = create_meta(fdp, file);
	else
		res = read_meta(fdp, file);

	if (res == TEE_SUCCESS) {
		*fh = (struct tee_file_handle *)fdp;
	} else {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		if (fdp->fd != -1)
			tee_fs_rpc_close(fdp->fd);
		if (create)
			tee_fs_rpc_remove(file);
		free(fdp);
	}
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result ree_fs_open(const char *file, struct tee_file_handle **fh)
{
	return open_internal(file, false, fh);
}

static TEE_Result ree_fs_create(const char *file, struct tee_file_handle **fh)
{
	return open_internal(file, true, fh);
}

static void ree_fs_close(struct tee_file_handle **fh)
{
	struct tee_fs_fd *fdp = (struct tee_fs_fd *)*fh;

	if (fdp) {
		tee_fs_rpc_close(fdp->fd);
		free(fdp);
		*fh = NULL;
	}
}

static TEE_Result ree_fs_seek(struct tee_file_handle *fh, int32_t offset,
			      TEE_Whence whence, int32_t *new_offs)
{
	TEE_Result res;
	tee_fs_off_t new_pos;
	size_t filelen;
	struct tee_fs_fd *fdp = (struct tee_fs_fd *)fh;

	filelen = fdp->meta.info.length;

	switch (whence) {
	case TEE_DATA_SEEK_SET:
		new_pos = offset;
		break;

	case TEE_DATA_SEEK_CUR:
		new_pos = fdp->pos + offset;
		break;

	case TEE_DATA_SEEK_END:
		new_pos = filelen + offset;
		break;

	default:
		res = TEE_ERROR_BAD_PARAMETERS;
		goto exit;
	}

	if (new_pos < 0)
		new_pos = 0;

	if (new_pos > TEE_DATA_MAX_POSITION) {
		EMSG(ERR_MSG_BAD_PARAMETERS ": %lld\n", new_pos);
		res = TEE_ERROR_BAD_PARAMETERS;
		goto exit;
	}

	fdp->pos = new_pos;
	if (new_offs)
		*new_offs = new_pos;
	DMSG("fdp->pos: %ld\n", (uint32_t)new_pos);
	res = TEE_SUCCESS;
exit:
	return res;
}

/*
 * To ensure atomic truncate operation, we can:
 *
 *  - update file length to new length
 *  - commit new meta
 *
 * To ensure atomic extend operation, we can:
 *
 *  - update file length to new length
 *  - allocate and fill zero data to new blocks
 *  - commit new meta
 *
 * Any failure before committing new meta is considered as
 * update failed, and the file content will not be updated
 */
static TEE_Result ree_fs_ftruncate_internal(struct tee_fs_fd *fdp,
					    tee_fs_off_t new_file_len)
{
	TEE_Result res;
	size_t old_file_len = fdp->meta.info.length;
	struct tee_fs_file_meta new_meta;

	if (new_file_len > MAX_FILE_SIZE) {
		EMSG(ERR_MSG_BAD_PARAMETERS ": %lld\n", new_file_len);
		return TEE_ERROR_BAD_PARAMETERS;
	}

	new_meta = fdp->meta;
	new_meta.info.length = new_file_len;

	if ((size_t)new_file_len > old_file_len) {
		size_t ext_len = new_file_len - old_file_len;
		int orig_pos = fdp->pos;

		fdp->pos = old_file_len;
		res = out_of_place_write(fdp, NULL, ext_len, &new_meta);
		fdp->pos = orig_pos;
		if (res != TEE_SUCCESS) {
			EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
			return res;
		}
	}

	return commit_meta_file(fdp, &new_meta);
}

static TEE_Result ree_fs_read(struct tee_file_handle *fh, void *buf,
			      size_t *len)
{
	TEE_Result res;
	int start_block_num;
	int end_block_num;
	size_t remain_bytes;
	uint8_t *data_ptr = buf;
	uint8_t block[BLOCK_SIZE];
	struct tee_fs_fd *fdp = (struct tee_fs_fd *)fh;

	remain_bytes = *len;
	if ((fdp->pos + remain_bytes) < remain_bytes ||
	    fdp->pos > (tee_fs_off_t)fdp->meta.info.length)
		remain_bytes = 0;
	else if (fdp->pos + (tee_fs_off_t)remain_bytes >
		(tee_fs_off_t)fdp->meta.info.length)
		remain_bytes = fdp->meta.info.length - fdp->pos;

	*len = remain_bytes;
	if (!remain_bytes) {
		res = TEE_SUCCESS;
		goto exit;
	}

	start_block_num = pos_to_block_num(fdp->pos);
	end_block_num = pos_to_block_num(fdp->pos + remain_bytes - 1);

	while (start_block_num <= end_block_num) {
		tee_fs_off_t offset = fdp->pos % BLOCK_SIZE;
		size_t size_to_read = MIN(remain_bytes, (size_t)BLOCK_SIZE);

		if (size_to_read + offset > BLOCK_SIZE)
			size_to_read = BLOCK_SIZE - offset;

		res = read_block(fdp, start_block_num, block);
		if (res != TEE_SUCCESS) {
			EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
			if (res == TEE_ERROR_MAC_INVALID)
				res = TEE_ERROR_CORRUPT_OBJECT;
			goto exit;
		}

		memcpy(data_ptr, block + offset, size_to_read);

		data_ptr += size_to_read;
		remain_bytes -= size_to_read;
		fdp->pos += size_to_read;

		start_block_num++;
	}
	res = TEE_SUCCESS;
exit:
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

/*
 * To ensure atomicity of write operation, we need to
 * do the following steps:
 * (The sequence of operations is very important)
 *
 *  - Create a new backup version of meta file as a copy
 *    of current meta file.
 *  - For each blocks to write:
 *    - Create new backup version for current block.
 *    - Write data to new backup version.
 *    - Update the new meta file accordingly.
 *  - Write the new meta file.
 *
 * (Any failure in above steps is considered as update failed,
 *  and the file content will not be updated)
 */
static TEE_Result ree_fs_write(struct tee_file_handle *fh, const void *buf,
			       size_t len)
{
	TEE_Result res;
	struct tee_fs_file_meta new_meta;
	struct tee_fs_fd *fdp = (struct tee_fs_fd *)fh;
	size_t file_size;

	if (!len)
		return TEE_SUCCESS;

	file_size = fdp->meta.info.length;

	if ((fdp->pos + len) > MAX_FILE_SIZE || (fdp->pos + len) < len) {
		EMSG(ERR_MSG_BAD_PARAMETERS ": %lld\n", fdp->pos + len);
		res = TEE_ERROR_BAD_PARAMETERS;
		goto exit;
	}

	if (file_size < (size_t)fdp->pos) {
		DMSG("ftruncate, pos: %zd\n", (size_t)fdp->pos);
		res = ree_fs_ftruncate_internal(fdp, fdp->pos);
		if (res != TEE_SUCCESS) {
			EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
			goto exit;
		}
	}

	new_meta = fdp->meta;
	DMSG("out of place write, len: %zd\n", len);
	res = out_of_place_write(fdp, buf, len, &new_meta);
	if (res != TEE_SUCCESS) {
		EMSG(ERR_MSG_GENERIC ": 0x%08lx\n", res);
		goto exit;
	}

	res = commit_meta_file(fdp, &new_meta);
exit:
	if (res) {
		DMSG("res: 0x%08lx\n", res);
	}
	return res;
}

static TEE_Result ree_fs_rename(const char *old, const char *new,
				bool overwrite)
{
	return tee_fs_rpc_rename(old, new, overwrite);
}

static TEE_Result ree_fs_remove(const char *file)
{
	return tee_fs_rpc_remove(file);
}

static TEE_Result ree_fs_truncate(struct tee_file_handle *fh, size_t len)
{
	TEE_Result res;
	struct tee_fs_fd *fdp = (struct tee_fs_fd *)fh;
	res = ree_fs_ftruncate_internal(fdp, len);

	return res;
}

static TEE_Result ree_fs_fsync(struct tee_file_handle **fh)
{
	TEE_Result res = TEE_SUCCESS;
	struct tee_fs_fd *fdp = (struct tee_fs_fd *)*fh;

	if (fdp) {
		res = tee_fs_rpc_fsync(fdp->fd);
	}
	return res;
}

const struct tee_file_operations ree_fs_ops = {
	.open = ree_fs_open,
	.create = ree_fs_create,
	.close = ree_fs_close,
	.read = ree_fs_read,
	.write = ree_fs_write,
	.seek = ree_fs_seek,
	.truncate = ree_fs_truncate,
	.rename = ree_fs_rename,
	.remove = ree_fs_remove,
	.opendir =  NULL, /* ree_fs_opendir_rpc */
	.closedir = NULL, /* ree_fs_closedir_rpc */
	.readdir = NULL, /* ree_fs_readdir_rpc */
	.fsync = ree_fs_fsync,
};
