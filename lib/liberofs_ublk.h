/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
/*
 * Copyright (C) 2026 Tencent, Inc.
 *             http://www.tencent.com/
 */
#ifndef __EROFS_LIB_LIBEROFS_UBLK_H
#define __EROFS_LIB_LIBEROFS_UBLK_H

#include "erofs/defs.h"

#define EROFS_UBLK_DEF_NR_HW_QUEUES	1
#define EROFS_UBLK_DEF_QUEUE_DEPTH	128
#define EROFS_UBLK_DEF_MAX_IO_BUF_BYTES	(512 * 1024)
#define EROFS_UBLK_DEF_BLK_BITS		12	/* 4KB */

#define EROFS_UBLK_F_UNPRIVILEGED	(1U << 0)
#define EROFS_UBLK_F_USER_RECOVERY	(1U << 1)
#define EROFS_UBLK_F_USER_COPY		(1U << 2)
#define EROFS_UBLK_F_ASYNC		(1U << 3)

#define EROFS_UBLK_OP_READ		0
#define EROFS_UBLK_OP_WRITE		1
#define EROFS_UBLK_OP_FLUSH		2
#define EROFS_UBLK_OP_DISCARD		3
#define EROFS_UBLK_OP_WRITE_ZEROES	5

#define EROFS_UBLK_IO_F_FUA		(1U << 13)
#define EROFS_UBLK_IO_F_NOUNMAP		(1U << 15)

#define EROFS_UBLK_IO_ASYNC		1

struct erofs_ublk_dev_info {
	u16 nr_hw_queues;
	u16 queue_depth;
	u32 max_io_buf_bytes;
	u32 dev_id;
	u64 dev_size;
	u8 blkbits;
	u8 reserved[3];
	u32 flags;
};

struct erofs_ublk_request {
	u16 q_id;
	u16 tag;
	u8 op;
	u8 reserved;
	u16 flags;
	u64 start_sector;
	u32 nr_sectors;
	u32 reserved2;
	void *buf;
	int result;
};

struct erofs_ublk_aio_ctx {
	u16 q_id;
	u16 tag;
	int dev_id;
	void *priv;
};

typedef int (*erofs_ublk_io_handler_t)(void *ctx,
				       struct erofs_ublk_request *req);

int erofs_ublk_init(void);
void erofs_ublk_exit(void);
int erofs_ublk_is_supported(void);
int erofs_ublk_devscan(void);

int erofs_ublk_create_dev(const struct erofs_ublk_dev_info *info,
			  erofs_ublk_io_handler_t handler,
			  void *handler_ctx);
int erofs_ublk_start(int dev_id);
void erofs_ublk_set_ready_fd(int dev_id, int fd);
int erofs_ublk_stop(int dev_id);
int erofs_ublk_set_sig_handler(int dev_id);
void erofs_ublk_destroy(int dev_id);

int erofs_ublk_get_dev_path(int dev_id, char *buf, size_t buflen);
const char *erofs_ublk_op_name(int op);
int erofs_ublk_del_dev_by_id(int dev_id);

int erofs_ublk_recover_dev(int dev_id,
			   erofs_ublk_io_handler_t handler,
			   void *handler_ctx);
int erofs_ublk_complete_recovery(int dev_id);
int erofs_ublk_is_recoverable(int dev_id);

int erofs_ublk_aio_get_ctx(int dev_id,
			   const struct erofs_ublk_request *req,
			   struct erofs_ublk_aio_ctx **ctx);
int erofs_ublk_aio_complete(struct erofs_ublk_aio_ctx *ctx,
			    int result);
void *erofs_ublk_aio_get_buf(struct erofs_ublk_aio_ctx *ctx);

#endif
