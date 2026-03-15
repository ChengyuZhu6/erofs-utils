/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
/*
 * Copyright (C) 2026 Tencent, Inc.
 *             http://www.tencent.com/
 */
#ifndef __EROFS_LIB_LIBEROFS_UBLK_H
#define __EROFS_LIB_LIBEROFS_UBLK_H

#include "erofs/defs.h"

#define EROFS_UBLK_F_UNPRIVILEGED	(1U << 0)
#define EROFS_UBLK_F_USER_RECOVERY	(1U << 1)

#define EROFS_UBLK_OP_READ		0

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
	u8 op;
	u64 start_sector;
	u32 nr_sectors;
	void *buf;
	int result;
};

typedef int (*erofs_ublk_io_handler_t)(void *ctx,
				       struct erofs_ublk_request *req);

int erofs_ublk_init(void);

int erofs_ublk_create_dev(const struct erofs_ublk_dev_info *info,
			  erofs_ublk_io_handler_t handler,
			  void *handler_ctx);
int erofs_ublk_start(int dev_id, int ready_fd);
void erofs_ublk_destroy(int dev_id);

int erofs_ublk_del_dev_by_id(int dev_id);

int erofs_ublk_recover_dev(int dev_id,
			   erofs_ublk_io_handler_t handler,
			   void *handler_ctx);
int erofs_ublk_is_recoverable(int dev_id);

#endif
