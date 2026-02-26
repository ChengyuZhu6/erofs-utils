// SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0
/*
 * Copyright (C) 2026 Tencent, Inc.
 *             http://www.tencent.com/
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/eventfd.h>
#include <sys/prctl.h>
#include <unistd.h>
#include <linux/io_uring.h>

#include "erofs/io.h"
#include "erofs/err.h"
#include "erofs/print.h"
#include "liberofs_ublk.h"

#ifdef HAVE_LIBURING
#include <liburing.h>
#endif

/*
 * ublk UAPI definitions - copied from linux/ublk_cmd.h
 * These are the minimal definitions needed for ublk operation.
 */

/* Control commands */
#define UBLK_CMD_GET_QUEUE_AFFINITY	0x01
#define UBLK_CMD_GET_DEV_INFO		0x02
#define UBLK_CMD_ADD_DEV		0x04
#define UBLK_CMD_DEL_DEV		0x05
#define UBLK_CMD_START_DEV		0x06
#define UBLK_CMD_STOP_DEV		0x07
#define UBLK_CMD_SET_PARAMS		0x08
#define UBLK_CMD_GET_PARAMS		0x09
#define UBLK_CMD_START_USER_RECOVERY	0x10
#define UBLK_CMD_END_USER_RECOVERY	0x11

/* IO commands */
#define UBLK_IO_FETCH_REQ		0x20
#define UBLK_IO_COMMIT_AND_FETCH_REQ	0x21
#define UBLK_IO_NEED_GET_DATA		0x22

/* Encoded commands using ioctl style */
#define UBLK_U_CMD_GET_QUEUE_AFFINITY	\
	_IOR('u', UBLK_CMD_GET_QUEUE_AFFINITY, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_DEV_INFO		\
	_IOR('u', UBLK_CMD_GET_DEV_INFO, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_ADD_DEV		\
	_IOWR('u', UBLK_CMD_ADD_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_DEL_DEV		\
	_IOWR('u', UBLK_CMD_DEL_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_START_DEV		\
	_IOWR('u', UBLK_CMD_START_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_STOP_DEV		\
	_IOWR('u', UBLK_CMD_STOP_DEV, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_SET_PARAMS		\
	_IOWR('u', UBLK_CMD_SET_PARAMS, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_GET_PARAMS		\
	_IOR('u', UBLK_CMD_GET_PARAMS, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_START_USER_RECOVERY	\
	_IOWR('u', UBLK_CMD_START_USER_RECOVERY, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_END_USER_RECOVERY	\
	_IOWR('u', UBLK_CMD_END_USER_RECOVERY, struct ublksrv_ctrl_cmd)
#define UBLK_U_CMD_DEL_DEV_ASYNC	\
	_IOR('u', 0x14, struct ublksrv_ctrl_cmd)

#define UBLK_U_IO_FETCH_REQ		\
	_IOWR('u', UBLK_IO_FETCH_REQ, struct ublksrv_io_cmd)
#define UBLK_U_IO_COMMIT_AND_FETCH_REQ	\
	_IOWR('u', UBLK_IO_COMMIT_AND_FETCH_REQ, struct ublksrv_io_cmd)
#define UBLK_U_IO_NEED_GET_DATA		\
	_IOWR('u', UBLK_IO_NEED_GET_DATA, struct ublksrv_io_cmd)

/* Feature flags */
#define UBLK_F_SUPPORT_ZERO_COPY	(1ULL << 0)
#define UBLK_F_URING_CMD_COMP_IN_TASK	(1ULL << 1)
#define UBLK_F_NEED_GET_DATA		(1ULL << 2)
#define UBLK_F_USER_RECOVERY		(1ULL << 3)
#define UBLK_F_USER_RECOVERY_REISSUE	(1ULL << 4)
#define UBLK_F_UNPRIVILEGED_DEV		(1ULL << 5)
#define UBLK_F_CMD_IOCTL_ENCODE		(1ULL << 6)
#define UBLK_F_USER_COPY		(1ULL << 7)
#define UBLK_F_ZONED			(1ULL << 8)

/* Device state */
#define UBLK_S_DEV_DEAD		0
#define UBLK_S_DEV_LIVE		1
#define UBLK_S_DEV_QUIESCED	2
#define UBLK_S_DEV_FAIL_IO	3

/* IO result codes */
#define UBLK_IO_RES_OK			0
#define UBLK_IO_RES_NEED_GET_DATA	1
#define UBLK_IO_RES_ABORT		(-ENODEV)

/* Buffer offsets for mmap */
#define UBLKSRV_CMD_BUF_OFFSET		0
#define UBLKSRV_IO_BUF_OFFSET		0x80000000

/* Limits */
#define UBLK_MAX_QUEUE_DEPTH		4096
#define UBLK_MAX_NR_QUEUES		32

/* USER_COPY position encoding: pos = UBLKSRV_IO_BUF_OFFSET + (q_id << 41) + (tag << 25) + offset */
#define UBLK_IO_BUF_BITS		25
#define UBLK_IO_BUF_BITS_MASK		((1ULL << UBLK_IO_BUF_BITS) - 1)
#define UBLK_TAG_OFF			UBLK_IO_BUF_BITS
#define UBLK_TAG_BITS			16
#define UBLK_QID_OFF			(UBLK_TAG_OFF + UBLK_TAG_BITS)

/* IO operations */
#define UBLK_IO_OP_READ			0
#define UBLK_IO_OP_WRITE		1
#define UBLK_IO_OP_FLUSH		2
#define UBLK_IO_OP_DISCARD		3
#define UBLK_IO_OP_WRITE_SAME		4
#define UBLK_IO_OP_WRITE_ZEROES		5
#define UBLK_IO_OP_ZONE_OPEN		10
#define UBLK_IO_OP_ZONE_CLOSE		11
#define UBLK_IO_OP_ZONE_FINISH		12
#define UBLK_IO_OP_ZONE_APPEND		13
#define UBLK_IO_OP_ZONE_RESET		15

/* IO flags */
#define UBLK_IO_F_FUA			(1U << 13)
#define UBLK_IO_F_NOUNMAP		(1U << 15)
#define UBLK_IO_F_SWAP			(1U << 16)

/* Control command structure */
struct ublksrv_ctrl_cmd {
	__u32 dev_id;
	__u16 queue_id;
	__u16 len;
	__u64 addr;
	__u64 data[1];
	__u16 dev_path_len;
	__u16 pad;
	__u32 reserved;
};

/* Device info structure */
struct ublksrv_ctrl_dev_info {
	__u16 nr_hw_queues;
	__u16 queue_depth;
	__u16 state;
	__u16 pad0;
	__u32 max_io_buf_bytes;
	__u32 dev_id;
	__s32 ublksrv_pid;
	__u32 pad1;
	__u64 flags;
	__u64 ublksrv_flags;
	__u32 owner_uid;
	__u32 owner_gid;
	__u64 reserved1;
	__u64 reserved2;
};

/* IO command structure */
struct ublksrv_io_cmd {
	__u16 q_id;
	__u16 tag;
	__s32 result;
	union {
		__u64 addr;
		__u64 zone_append_lba;
	};
};

/* IO descriptor - shared memory from kernel */
struct ublksrv_io_desc {
	__u32 op_flags;
	union {
		__u32 nr_sectors;
		__u32 nr_zones;
	};
	__u64 start_sector;
	__u64 addr;
};

/* Parameter types */
#define UBLK_PARAM_TYPE_BASIC		(1 << 0)
#define UBLK_PARAM_TYPE_DISCARD		(1 << 1)
#define UBLK_PARAM_TYPE_DEVT		(1 << 2)
#define UBLK_PARAM_TYPE_ZONED		(1 << 3)

/* Basic parameters */
struct ublk_param_basic {
#define UBLK_ATTR_READ_ONLY		(1 << 0)
#define UBLK_ATTR_ROTATIONAL		(1 << 1)
#define UBLK_ATTR_VOLATILE_CACHE	(1 << 2)
#define UBLK_ATTR_FUA			(1 << 3)
	__u32 attrs;
	__u8 logical_bs_shift;
	__u8 physical_bs_shift;
	__u8 io_opt_shift;
	__u8 io_min_shift;
	__u32 max_sectors;
	__u32 chunk_sectors;
	__u64 dev_sectors;
	__u64 virt_boundary_mask;
};

/* Discard parameters */
struct ublk_param_discard {
	__u32 discard_alignment;
	__u32 discard_granularity;
	__u32 max_discard_sectors;
	__u32 max_write_zeroes_sectors;
	__u16 max_discard_segments;
	__u16 reserved0;
};

/* Device parameters */
struct ublk_param_devt {
	__u32 char_major;
	__u32 char_minor;
	__u32 disk_major;
	__u32 disk_minor;
};

/* Combined parameters */
struct ublk_params {
	__u32 len;
	__u32 types;
	struct ublk_param_basic basic;
	struct ublk_param_discard discard;
	struct ublk_param_devt devt;
};

/*
 * Internal structures
 */

/* IO state flags */
#define UBLKSRV_IO_FREE			(1U << 0)
#define UBLKSRV_NEED_FETCH_RQ		(1U << 1)
#define UBLKSRV_NEED_COMMIT_RQ_COMP	(1U << 2)
#define UBLKSRV_IO_NEED_GET_DATA	(1U << 3)
#define UBLKSRV_IO_ASYNC_PENDING	(1U << 4)  /* Async completion pending */

/* Queue state flags */
#define UBLKSRV_QUEUE_STOPPING		(1U << 0)
#define UBLKSRV_QUEUE_IDLE		(1U << 1)

/* Per-IO context */
struct erofs_ublk_io {
	unsigned int flags;
	int result;
	void *buf;
};

/* Per-queue context */
struct erofs_ublk_queue {
	int q_id;
	int q_depth;
	struct erofs_ublk_dev *dev;
	struct ublksrv_io_desc *io_cmd_buf;	/* mmap from kernel */
	struct erofs_ublk_io *ios;		/* per-tag IO state */
	void *io_buf;				/* contiguous IO buffer */
	size_t io_buf_size;
	pthread_t thread;
	unsigned int state;
	unsigned int cmd_inflight;		/* commands in flight */
	cpu_set_t *cpuset;			/* CPU affinity */
	int idle_ticks;
	int efd;				/* eventfd for wakeup/shutdown */
	int use_fixed_file;			/* using registered file */
	int use_defer_taskrun;			/* using DEFER_TASKRUN mode */
	pthread_barrier_t *init_barrier;	/* barrier for thread init sync */
#ifdef HAVE_LIBURING
	struct io_uring ring;
#endif
};

/* Device context */
struct erofs_ublk_dev {
	int ctrl_fd;				/* /dev/ublk-control fd */
	int cdev_fd;				/* /dev/ublkcN fd */
	struct ublksrv_ctrl_dev_info dev_info;
	struct ublk_params params;
	struct erofs_ublk_queue *queues;
	erofs_ublk_io_handler_t handler;
	void *handler_ctx;
	volatile int running;
	volatile int stop_requested;
	int async_enabled;			/* Async IO support enabled */
	int stop_efd;				/* eventfd for stop signal */
	int ready_fd;				/* fd to signal device readiness */
#ifdef HAVE_LIBURING
	struct io_uring ctrl_ring;		/* reusable control ring */
	int ctrl_ring_initialized;
#endif
};

/* Control device path */
#define UBLK_CTRL_DEV		"/dev/ublk-control"
#define UBLK_CDEV_FMT		"/dev/ublkc%d"
#define UBLK_BDEV_FMT		"/dev/ublkb%d"

/* Idle timeout in 100ms ticks (20 seconds) */
#define UBLK_IDLE_TIMEOUT_TICKS	200

/*
 * Global device pointer for signal handler
 * Only one device can have signal handling at a time
 */
static volatile struct erofs_ublk_dev *g_sig_dev = NULL;

/*
 * Helper functions
 */

static inline __u8 ublksrv_get_op(const struct ublksrv_io_desc *iod)
{
	return iod->op_flags & 0xff;
}

static inline __u32 ublksrv_get_flags(const struct ublksrv_io_desc *iod)
{
	return iod->op_flags >> 8;
}

static inline unsigned int ublk_cmd_buf_sz(int q_depth)
{
	unsigned int size = q_depth * sizeof(struct ublksrv_io_desc);
	unsigned int page_size = getpagesize();

	return (size + page_size - 1) & ~(page_size - 1);
}

static inline void *ublk_get_io_buf(struct erofs_ublk_queue *q, int tag)
{
	return (char *)q->io_buf + tag * q->dev->dev_info.max_io_buf_bytes;
}

/*
 * Calculate ublk position for USER_COPY mode pread/pwrite
 * This position is used to identify the specific IO buffer in /dev/ublkcN
 */
static inline __u64 ublk_pos(__u16 q_id, __u16 tag, __u32 offset)
{
	return UBLKSRV_IO_BUF_OFFSET +
		(((__u64)q_id) << UBLK_QID_OFF) |
		(((__u64)tag) << UBLK_TAG_OFF) |
		((__u64)(offset & UBLK_IO_BUF_BITS_MASK));
}

/*
 * Apply OOM protection to prevent the ublk daemon from being killed
 * by the kernel OOM killer. This is critical for block device stability.
 * Inspired by ublksrv_apply_oom_protection() in ublksrv.
 */
static void ublk_apply_oom_protection(void)
{
	char path[64];
	int fd;

	snprintf(path, sizeof(path), "/proc/%d/oom_score_adj", getpid());
	fd = open(path, O_RDWR);
	if (fd >= 0) {
		const char *val = "-1000";
		ssize_t ret = write(fd, val, strlen(val));
		if (ret != (ssize_t)strlen(val))
			erofs_dbg("failed to set oom_score_adj: %s",
				  strerror(errno));
		close(fd);
	}
}

/*
 * Set PR_SET_IO_FLUSHER to indicate this process handles IO for
 * block devices, which affects memory reclaim behavior.
 */
static void ublk_set_io_flusher(void)
{
#if defined(PR_SET_IO_FLUSHER)
	if (prctl(PR_SET_IO_FLUSHER, 0, 0, 0, 0) != 0)
		erofs_dbg("prctl(PR_SET_IO_FLUSHER) failed: %s",
			  strerror(errno));
#endif
}

/*
 * Initialize a reusable control io_uring ring
 */
#ifdef HAVE_LIBURING
static int ublk_ctrl_ring_init(struct io_uring *ring)
{
	struct io_uring_params p;

	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SQE128;

	return io_uring_queue_init_params(4, ring, &p);
}

/*
 * Send control command via io_uring using a provided ring.
 * If ring is NULL, creates a temporary one (for standalone use).
 */
static int ublk_ctrl_cmd_ring(struct io_uring *ring, int ctrl_fd,
			      __u32 cmd_op,
			      const struct ublksrv_ctrl_cmd *cmd_data)
{
	struct io_uring_sqe *sqe;
	struct io_uring_cqe *cqe;
	struct ublksrv_ctrl_cmd *cmd;
	int ret;

	sqe = io_uring_get_sqe(ring);
	if (!sqe)
		return -ENOMEM;

	/* Prepare uring command - use io_uring_prep_rw for basic setup */
	io_uring_prep_rw(IORING_OP_URING_CMD, sqe, ctrl_fd, NULL, 0, 0);
	sqe->cmd_op = cmd_op;

	/*
	 * For SQE128, the command data area starts at offset 48 (sqe->addr3).
	 * With liburing, sqe->cmd points to offset 48 in the 128-byte SQE.
	 */
	if (cmd_data) {
		cmd = (struct ublksrv_ctrl_cmd *)sqe->cmd;
		memcpy(cmd, cmd_data, sizeof(*cmd_data));
	}

	ret = io_uring_submit(ring);
	if (ret < 0) {
		erofs_err("io_uring_submit failed: %s", strerror(-ret));
		return ret;
	}

	ret = io_uring_wait_cqe(ring, &cqe);
	if (ret < 0) {
		erofs_err("io_uring_wait_cqe failed: %s", strerror(-ret));
		return ret;
	}

	ret = cqe->res;
	io_uring_cqe_seen(ring, cqe);
	return ret;
}

/*
 * Send control command via io_uring
 * Creates a temporary ring for standalone callers (e.g., del_dev_by_id)
 */
static int ublk_ctrl_cmd(int ctrl_fd, __u32 cmd_op,
			 const struct ublksrv_ctrl_cmd *cmd_data)
{
	struct io_uring ring;
	int ret;

	ret = ublk_ctrl_ring_init(&ring);
	if (ret < 0) {
		erofs_err("io_uring_queue_init failed: %s", strerror(-ret));
		return ret;
	}

	ret = ublk_ctrl_cmd_ring(&ring, ctrl_fd, cmd_op, cmd_data);
	io_uring_queue_exit(&ring);
	return ret;
}

/*
 * Send control command using device's persistent control ring
 */
static int ublk_dev_ctrl_cmd(struct erofs_ublk_dev *dev, __u32 cmd_op,
			     const struct ublksrv_ctrl_cmd *cmd_data)
{
	return ublk_ctrl_cmd_ring(&dev->ctrl_ring, dev->ctrl_fd,
				  cmd_op, cmd_data);
}

/*
 * Get queue CPU affinity from kernel
 */
static int ublk_get_queue_affinity(struct erofs_ublk_dev *dev, int q_id,
				   cpu_set_t *cpuset)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	int ret;

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = q_id;
	cmd.len = sizeof(cpu_set_t);
	cmd.addr = (__u64)(uintptr_t)cpuset;

	ret = ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_GET_QUEUE_AFFINITY, &cmd);
	if (ret < 0)
		erofs_dbg("GET_QUEUE_AFFINITY failed for q%d: %s",
			  q_id, strerror(-ret));
	return ret;
}

/*
 * Add ublk device
 */
static int ublk_add_dev(struct erofs_ublk_dev *dev,
			const struct erofs_ublk_dev_info *info)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	struct ublksrv_ctrl_dev_info *dev_info = &dev->dev_info;
	int ret;

	memset(dev_info, 0, sizeof(*dev_info));
	dev_info->nr_hw_queues = info->nr_hw_queues;
	dev_info->queue_depth = info->queue_depth;
	dev_info->max_io_buf_bytes = info->max_io_buf_bytes;
	dev_info->dev_id = info->dev_id;

	/* Required flags for modern ublk */
	dev_info->flags = UBLK_F_CMD_IOCTL_ENCODE |
			  UBLK_F_URING_CMD_COMP_IN_TASK;

	/* USER_COPY mode: we handle data copy ourselves */
	dev_info->flags |= UBLK_F_USER_COPY;

	if (info->flags & EROFS_UBLK_F_UNPRIVILEGED)
		dev_info->flags |= UBLK_F_UNPRIVILEGED_DEV;
	if (info->flags & EROFS_UBLK_F_USER_RECOVERY)
		dev_info->flags |= UBLK_F_USER_RECOVERY;

	cmd.dev_id = dev_info->dev_id;
	cmd.queue_id = (__u16)-1;
	cmd.len = sizeof(*dev_info);
	cmd.addr = (__u64)(uintptr_t)dev_info;

	ret = ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_ADD_DEV, &cmd);
	if (ret < 0) {
		erofs_err("UBLK_CMD_ADD_DEV failed: %s", strerror(-ret));
		return ret;
	}

	erofs_info("ublk device %d added (queues=%d, depth=%d, io_buf=%u)",
		   dev_info->dev_id, dev_info->nr_hw_queues,
		   dev_info->queue_depth, dev_info->max_io_buf_bytes);
	return 0;
}

/*
 * Delete ublk device
 */
static int ublk_del_dev(struct erofs_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {0};

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = (__u16)-1;

	return ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_DEL_DEV, &cmd);
}

/*
 * Set device parameters
 */
static int ublk_set_params(struct erofs_ublk_dev *dev,
			   const struct erofs_ublk_dev_info *info)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	struct ublk_params *params = &dev->params;
	int ret;

	memset(params, 0, sizeof(*params));
	params->len = sizeof(*params);
	params->types = UBLK_PARAM_TYPE_BASIC;

	/* For erofs, the device is read-only */
	params->basic.attrs = UBLK_ATTR_READ_ONLY;
	params->basic.logical_bs_shift = info->blkbits;
	params->basic.physical_bs_shift = info->blkbits;
	params->basic.io_opt_shift = info->blkbits;
	params->basic.io_min_shift = info->blkbits;
	params->basic.max_sectors = info->max_io_buf_bytes >> 9;
	params->basic.dev_sectors = info->dev_size >> 9;

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = (__u16)-1;
	cmd.len = sizeof(*params);
	cmd.addr = (__u64)(uintptr_t)params;

	ret = ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_SET_PARAMS, &cmd);
	if (ret < 0)
		erofs_err("UBLK_CMD_SET_PARAMS failed: %s", strerror(-ret));

	return ret;
}

/*
 * Start ublk device
 */
static int ublk_start_dev(struct erofs_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	int ret;

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = (__u16)-1;
	cmd.data[0] = getpid();

	ret = ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_START_DEV, &cmd);
	if (ret < 0)
		erofs_err("UBLK_CMD_START_DEV failed: %s", strerror(-ret));
	else
		erofs_info("ublk device /dev/ublkb%d started",
			   dev->dev_info.dev_id);

	return ret;
}

/*
 * Stop ublk device
 */
static int ublk_stop_dev(struct erofs_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {0};

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = (__u16)-1;

	return ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_STOP_DEV, &cmd);
}

/*
 * Start user recovery process
 * This should be called after device crash to reattach to existing device
 */
static int ublk_start_recovery(struct erofs_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	int ret;

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = (__u16)-1;

	ret = ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_START_USER_RECOVERY, &cmd);
	if (ret < 0)
		erofs_err("START_USER_RECOVERY failed: %s", strerror(-ret));
	else
		erofs_info("ublk device %d recovery started",
			   dev->dev_info.dev_id);

	return ret;
}

/*
 * End user recovery process
 * This should be called after handler and queues are re-initialized
 */
static int ublk_end_recovery(struct erofs_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	int ret;

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = (__u16)-1;
	cmd.data[0] = getpid();

	ret = ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_END_USER_RECOVERY, &cmd);
	if (ret < 0)
		erofs_err("END_USER_RECOVERY failed: %s", strerror(-ret));
	else
		erofs_info("ublk device %d recovery completed",
			   dev->dev_info.dev_id);

	return ret;
}

/*
 * Get device info (for recovery - reattach to existing device)
 */
static int ublk_get_dev_info(struct erofs_ublk_dev *dev, int dev_id)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	int ret;

	cmd.dev_id = dev_id;
	cmd.queue_id = (__u16)-1;
	cmd.len = sizeof(dev->dev_info);
	cmd.addr = (__u64)(uintptr_t)&dev->dev_info;

	ret = ublk_ctrl_cmd(dev->ctrl_fd, UBLK_U_CMD_GET_DEV_INFO, &cmd);
	if (ret < 0)
		erofs_err("GET_DEV_INFO failed: %s", strerror(-ret));

	return ret;
}

/*
 * Get device parameters (for recovery - restore device params)
 */
static int ublk_get_params(struct erofs_ublk_dev *dev)
{
	struct ublksrv_ctrl_cmd cmd = {0};
	int ret;

	dev->params.len = sizeof(dev->params);

	cmd.dev_id = dev->dev_info.dev_id;
	cmd.queue_id = (__u16)-1;
	cmd.len = sizeof(dev->params);
	cmd.addr = (__u64)(uintptr_t)&dev->params;

	ret = ublk_dev_ctrl_cmd(dev, UBLK_U_CMD_GET_PARAMS, &cmd);
	if (ret < 0)
		erofs_err("GET_PARAMS failed: %s", strerror(-ret));

	return ret;
}

/*
 * Build and submit IO command to kernel
 */
static int ublk_queue_io_cmd(struct erofs_ublk_queue *q, int tag)
{
	struct io_uring_sqe *sqe;
	struct ublksrv_io_cmd *cmd;
	struct erofs_ublk_io *io = &q->ios[tag];
	__u32 cmd_op;

	sqe = io_uring_get_sqe(&q->ring);
	if (!sqe)
		return -ENOMEM;

	/* Determine command type based on IO state */
	if (io->flags & UBLKSRV_IO_FREE)
		cmd_op = UBLK_U_IO_FETCH_REQ;
	else if (io->flags & UBLKSRV_IO_NEED_GET_DATA)
		cmd_op = UBLK_U_IO_NEED_GET_DATA;
	else
		cmd_op = UBLK_U_IO_COMMIT_AND_FETCH_REQ;

	/*
	 * Setup SQE manually for URING_CMD in SQE128 mode.
	 * Don't use io_uring_prep_rw as it clears addr3/cmd area.
	 */
	memset(sqe, 0, sizeof(*sqe) * 2);  /* Clear both 64-byte halves in SQE128 mode */

	sqe->opcode = IORING_OP_URING_CMD;
	sqe->fd = q->dev->cdev_fd;
	sqe->cmd_op = cmd_op;
	sqe->user_data = tag;

	/* Command data starts at offset 48 (sqe->cmd / sqe->addr3) */
	cmd = (struct ublksrv_io_cmd *)sqe->cmd;
	cmd->q_id = q->q_id;
	cmd->tag = tag;

	if (cmd_op == UBLK_U_IO_COMMIT_AND_FETCH_REQ)
		cmd->result = io->result;
	else
		cmd->result = 0;

	/*
	 * In USER_COPY mode:
	 * - FETCH_REQ: addr must be 0 (kernel doesn't use buffer address)
	 * - COMMIT_AND_FETCH_REQ/NEED_GET_DATA: addr is unused (we use pread/pwrite)
	 */
	cmd->addr = 0;

	/* Clear flags for next round */
	io->flags = 0;
	q->cmd_inflight++;

	return 0;
}

/*
 * Submit initial fetch requests for all tags
 */
static int ublk_submit_fetch_commands(struct erofs_ublk_queue *q)
{
	int i, ret;

	for (i = 0; i < q->q_depth; i++) {
		q->ios[i].flags = UBLKSRV_IO_FREE;
		ret = ublk_queue_io_cmd(q, i);
		if (ret < 0)
			return ret;
	}

	ret = io_uring_submit(&q->ring);
	if (ret < 0) {
		erofs_err("io_uring_submit (fetch) failed: %s", strerror(-ret));
		return ret;
	}

	return 0;
}

/*
 * Copy data from ublk device in USER_COPY mode (for WRITE operations)
 * Before handler processes write, we need to read data from /dev/ublkcN
 */
static int ublk_user_copy_read(struct erofs_ublk_queue *q, int tag, size_t len)
{
	__u64 pos = ublk_pos(q->q_id, tag, 0);
	void *buf = ublk_get_io_buf(q, tag);
	ssize_t ret;

	ret = pread(q->dev->cdev_fd, buf, len, pos);
	if (ret < 0) {
		erofs_err("user_copy pread failed: %s", strerror(errno));
		return -errno;
	}
	if ((size_t)ret != len) {
		erofs_err("user_copy pread short: %zd/%zu", ret, len);
		return -EIO;
	}
	return 0;
}

/*
 * Copy data to ublk device in USER_COPY mode (for READ completion)
 * After handler fills the buffer, we need to write it to /dev/ublkcN
 */
static int ublk_user_copy_write(struct erofs_ublk_queue *q, int tag, size_t len)
{
	__u64 pos = ublk_pos(q->q_id, tag, 0);
	void *buf = ublk_get_io_buf(q, tag);
	ssize_t ret;

	ret = pwrite(q->dev->cdev_fd, buf, len, pos);
	if (ret < 0) {
		erofs_err("user_copy pwrite failed: %s", strerror(errno));
		return -errno;
	}
	if ((size_t)ret != len) {
		erofs_err("user_copy pwrite short: %zd/%zu", ret, len);
		return -EIO;
	}
	return 0;
}

/*
 * Process one IO request from kernel
 * Returns: 0 for sync completion, EROFS_UBLK_IO_ASYNC for async, negative on error
 */
static int ublk_handle_io(struct erofs_ublk_queue *q, int tag)
{
	struct erofs_ublk_dev *dev = q->dev;
	const struct ublksrv_io_desc *iod = &q->io_cmd_buf[tag];
	struct erofs_ublk_io *io = &q->ios[tag];
	struct erofs_ublk_request req;
	int ret;

	/* Build request structure */
	req.q_id = q->q_id;
	req.tag = tag;
	req.op = ublksrv_get_op(iod);
	req.flags = ublksrv_get_flags(iod);
	req.start_sector = iod->start_sector;
	req.nr_sectors = iod->nr_sectors;
	req.buf = ublk_get_io_buf(q, tag);
	req.result = 0;

	/* Call user handler */
	if (dev->handler) {
		ret = dev->handler(dev->handler_ctx, &req);

		/* Check for async completion */
		if (ret == EROFS_UBLK_IO_ASYNC && dev->async_enabled) {
			io->flags = UBLKSRV_IO_ASYNC_PENDING;
			return EROFS_UBLK_IO_ASYNC;
		}

		if (ret < 0) {
			io->result = ret;
		} else {
			size_t io_bytes = req.nr_sectors << 9;

			/*
			 * In USER_COPY mode, for READ operations we need to
			 * copy data from our buffer to the kernel via pwrite()
			 * on /dev/ublkcN.
			 */
			if (req.op == UBLK_IO_OP_READ && io_bytes > 0) {
				ret = ublk_user_copy_write(q, tag, io_bytes);
				if (ret < 0) {
					io->result = ret;
				} else {
					io->result = io_bytes;
				}
			} else {
				io->result = io_bytes;
			}
		}
	} else {
		io->result = -ENOSYS;
	}

	/* Mark as needing commit */
	io->flags = UBLKSRV_NEED_COMMIT_RQ_COMP;

	return 0;
}

/*
 * Handle completion queue events
 */
static int ublk_handle_cqe(struct erofs_ublk_queue *q,
			   struct io_uring_cqe *cqe)
{
	int tag = cqe->user_data;
	struct erofs_ublk_io *io = &q->ios[tag];
	int ret = cqe->res;

	q->cmd_inflight--;

	if (ret == UBLK_IO_RES_ABORT) {
		/* Device is being stopped, don't fetch more */
		return -ENODEV;
	}

	if (ret == UBLK_IO_RES_NEED_GET_DATA) {
		/*
		 * WRITE operation: kernel has data ready, fetch it via pread()
		 * This is required in USER_COPY mode for WRITE operations.
		 */
		const struct ublksrv_io_desc *iod = &q->io_cmd_buf[tag];
		size_t io_bytes = (size_t)iod->nr_sectors << 9;

		ret = ublk_user_copy_read(q, tag, io_bytes);
		if (ret < 0) {
			/* Failed to read data, return error */
			io->result = ret;
			io->flags = UBLKSRV_NEED_COMMIT_RQ_COMP;
			return ublk_queue_io_cmd(q, tag);
		}

		io->flags = UBLKSRV_IO_NEED_GET_DATA;
		return ublk_queue_io_cmd(q, tag);
	}

	if (ret < 0) {
		erofs_err("queue %d tag %d IO error: %s",
			  q->q_id, tag, strerror(-ret));
		io->flags = UBLKSRV_IO_FREE;
		return ublk_queue_io_cmd(q, tag);
	}

	/* Got a new IO request from kernel, process it */
	ret = ublk_handle_io(q, tag);

	/* Check if async - don't submit completion yet */
	if (ret == EROFS_UBLK_IO_ASYNC)
		return 0;

	/* Submit result and fetch next */
	return ublk_queue_io_cmd(q, tag);
}

/*
 * Enter idle state - release memory back to system
 * Inspired by ublksrv's idle state optimization
 */
static void ublk_queue_enter_idle(struct erofs_ublk_queue *q)
{
	if (q->state & UBLKSRV_QUEUE_IDLE)
		return;

	q->state |= UBLKSRV_QUEUE_IDLE;

	/* Release IO buffer memory back to system */
	if (q->io_buf && q->io_buf_size > 0)
		madvise(q->io_buf, q->io_buf_size, MADV_DONTNEED);

	erofs_dbg("queue %d entered idle state", q->q_id);
}

/*
 * Exit idle state
 */
static void ublk_queue_exit_idle(struct erofs_ublk_queue *q)
{
	if (!(q->state & UBLKSRV_QUEUE_IDLE))
		return;

	q->state &= ~UBLKSRV_QUEUE_IDLE;
	q->idle_ticks = 0;
	erofs_dbg("queue %d exited idle state", q->q_id);
}

/*
 * Queue thread main loop
 */
static void *ublk_queue_thread(void *arg)
{
	struct erofs_ublk_queue *q = arg;
	struct io_uring_cqe *cqe;
	struct __kernel_timespec ts = {
		.tv_sec = 0,
		.tv_nsec = 100000000,	/* 100ms */
	};
	unsigned int head;
	int ret, nr_events;

	/* Set CPU affinity if available */
	if (q->cpuset)
		sched_setaffinity(0, sizeof(cpu_set_t), q->cpuset);

	/* Set high priority for IO thread */
	setpriority(PRIO_PROCESS, 0, -20);

	/* Mark as IO flusher for memory reclaim handling */
	ublk_set_io_flusher();

	/* Submit initial fetch requests */
	ret = ublk_submit_fetch_commands(q);
	if (ret < 0) {
		erofs_err("Failed to submit fetch commands: %s", strerror(-ret));
		if (q->init_barrier)
			pthread_barrier_wait(q->init_barrier);
		return NULL;
	}

	/* Signal that this thread has initialized and submitted fetch commands */
	if (q->init_barrier)
		pthread_barrier_wait(q->init_barrier);

	erofs_info("queue %d thread started (tid=%d, fixed_file=%d, defer_taskrun=%d)",
		   q->q_id, gettid(), q->use_fixed_file, q->use_defer_taskrun);

	/* Main IO loop */
	while (!(q->state & UBLKSRV_QUEUE_STOPPING)) {
		/*
		 * For DEFER_TASKRUN mode, need to call io_uring_get_events()
		 * to run task work before waiting for CQEs.
		 */
		if (q->use_defer_taskrun)
			io_uring_get_events(&q->ring);

		ret = io_uring_submit_and_wait_timeout(&q->ring, &cqe, 1,
						       &ts, NULL);
		if (ret == -ETIME) {
			/* Timeout - check for idle */
			if (++q->idle_ticks >= UBLK_IDLE_TIMEOUT_TICKS)
				ublk_queue_enter_idle(q);
			continue;
		}

		if (ret < 0) {
			if (ret == -EINTR)
				continue;
			erofs_err("io_uring_submit_and_wait_timeout: %s",
				  strerror(-ret));
			break;
		}

		/* Got events - exit idle if needed */
		ublk_queue_exit_idle(q);

		/* Process all completions */
		nr_events = 0;
		io_uring_for_each_cqe(&q->ring, head, cqe) {
			ret = ublk_handle_cqe(q, cqe);
			nr_events++;
			if (ret == -ENODEV) {
				q->state |= UBLKSRV_QUEUE_STOPPING;
				break;
			}
		}

		if (nr_events)
			io_uring_cq_advance(&q->ring, nr_events);

		/* Submit any pending commands */
		io_uring_submit(&q->ring);
	}

	erofs_info("queue %d thread exiting", q->q_id);
	q->dev->stop_requested = 1;
	return NULL;
}

/*
 * Initialize a queue
 */
static int ublk_init_queue(struct erofs_ublk_dev *dev, int q_id)
{
	struct erofs_ublk_queue *q = &dev->queues[q_id];
	struct io_uring_params p;
	unsigned int cmd_buf_size;
	int ret;

	q->q_id = q_id;
	q->q_depth = dev->dev_info.queue_depth;
	q->dev = dev;
	q->state = 0;
	q->cmd_inflight = 0;
	q->idle_ticks = 0;
	q->efd = -1;
	q->use_fixed_file = 0;
	q->use_defer_taskrun = 0;

	/* Create eventfd for signaling (e.g., graceful shutdown) */
	q->efd = eventfd(0, EFD_CLOEXEC);
	if (q->efd < 0)
		erofs_dbg("eventfd creation failed: %s", strerror(errno));

	/* Try to get CPU affinity */
	q->cpuset = malloc(sizeof(cpu_set_t));
	if (q->cpuset) {
		CPU_ZERO(q->cpuset);
		ret = ublk_get_queue_affinity(dev, q_id, q->cpuset);
		if (ret < 0) {
			free(q->cpuset);
			q->cpuset = NULL;
		}
	}

	/* Map IO command buffer (shared with kernel) */
	cmd_buf_size = ublk_cmd_buf_sz(q->q_depth);
	q->io_cmd_buf = mmap(NULL, cmd_buf_size, PROT_READ,
			     MAP_SHARED | MAP_POPULATE, dev->cdev_fd,
			     UBLKSRV_CMD_BUF_OFFSET +
			     q_id * (UBLK_MAX_QUEUE_DEPTH *
				     sizeof(struct ublksrv_io_desc)));
	if (q->io_cmd_buf == MAP_FAILED) {
		erofs_err("mmap io_cmd_buf failed: %s", strerror(errno));
		ret = -errno;
		goto err_close_efd;
	}

	/* Allocate per-tag IO state */
	q->ios = calloc(q->q_depth, sizeof(struct erofs_ublk_io));
	if (!q->ios) {
		ret = -ENOMEM;
		goto err_unmap_cmd;
	}

	/* Allocate contiguous IO buffer for all tags */
	q->io_buf_size = (size_t)q->q_depth * dev->dev_info.max_io_buf_bytes;
	q->io_buf = mmap(NULL, q->io_buf_size, PROT_READ | PROT_WRITE,
			 MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
	if (q->io_buf == MAP_FAILED) {
		erofs_err("mmap io_buf failed: %s", strerror(errno));
		ret = -errno;
		goto err_free_ios;
	}

	/*
	 * Initialize io_uring with SQE128 (required for ublk uring commands)
	 * Start with minimal flags for debugging.
	 */
	memset(&p, 0, sizeof(p));
	p.flags = IORING_SETUP_SQE128;

	ret = io_uring_queue_init_params(q->q_depth * 2, &q->ring, &p);
	if (ret < 0) {
		erofs_err("io_uring_queue_init failed: %s", strerror(-ret));
		goto err_unmap_buf;
	}
	q->use_defer_taskrun = 0;
	q->use_fixed_file = 0;

	return 0;

err_unmap_buf:
	munmap(q->io_buf, q->io_buf_size);
err_free_ios:
	free(q->ios);
err_unmap_cmd:
	munmap(q->io_cmd_buf, cmd_buf_size);
err_close_efd:
	if (q->efd >= 0)
		close(q->efd);
	free(q->cpuset);
	return ret;
}

/*
 * Cleanup a queue
 */
static void ublk_cleanup_queue(struct erofs_ublk_dev *dev, int q_id)
{
	struct erofs_ublk_queue *q = &dev->queues[q_id];

	/* Signal thread to stop */
	q->state |= UBLKSRV_QUEUE_STOPPING;

	/* Wake up the thread if blocked on io_uring via eventfd */
	if (q->efd >= 0) {
		uint64_t val = 1;
		if (write(q->efd, &val, sizeof(val)) != sizeof(val))
			erofs_dbg("eventfd write failed");
	}

	/* Wait for thread to exit */
	if (q->thread) {
		pthread_join(q->thread, NULL);
		q->thread = 0;
	}

	io_uring_unregister_ring_fd(&q->ring);
	io_uring_queue_exit(&q->ring);

	if (q->io_buf && q->io_buf != MAP_FAILED)
		munmap(q->io_buf, q->io_buf_size);

	free(q->ios);

	if (q->io_cmd_buf && q->io_cmd_buf != MAP_FAILED)
		munmap(q->io_cmd_buf, ublk_cmd_buf_sz(q->q_depth));

	if (q->efd >= 0)
		close(q->efd);

	free(q->cpuset);
}
#endif /* HAVE_LIBURING */

/*
 * Public API implementations
 */

int erofs_ublk_init(void)
{
#ifndef HAVE_LIBURING
	return -EOPNOTSUPP;
#else
	return 0;
#endif
}

void erofs_ublk_exit(void)
{
}

int erofs_ublk_is_supported(void)
{
#ifndef HAVE_LIBURING
	return 0;
#else
	int fd;

	fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (fd < 0)
		return 0;
	close(fd);
	return 1;
#endif
}

int erofs_ublk_devscan(void)
{
	char path[64];
	int i;

	for (i = 0; i < 128; i++) {
		snprintf(path, sizeof(path), UBLK_CDEV_FMT, i);
		if (access(path, F_OK) != 0)
			return i;
	}
	return -ENOSPC;
}

int erofs_ublk_create_dev(const struct erofs_ublk_dev_info *info,
			  erofs_ublk_io_handler_t handler,
			  void *handler_ctx,
			  struct erofs_ublk_dev **pdev)
{
#ifndef HAVE_LIBURING
	(void)info;
	(void)handler;
	(void)handler_ctx;
	(void)pdev;
	return -EOPNOTSUPP;
#else
	struct erofs_ublk_dev *dev;
	struct erofs_ublk_dev_info def_info;
	char cdev_path[64];
	int i, ret;

	if (!info) {
		memset(&def_info, 0, sizeof(def_info));
		def_info.nr_hw_queues = EROFS_UBLK_DEF_NR_HW_QUEUES;
		def_info.queue_depth = EROFS_UBLK_DEF_QUEUE_DEPTH;
		def_info.max_io_buf_bytes = EROFS_UBLK_DEF_MAX_IO_BUF_BYTES;
		def_info.blkbits = EROFS_UBLK_DEF_BLK_BITS;
		def_info.dev_id = (__u32)-1;
		info = &def_info;
	}

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return -ENOMEM;

	dev->handler = handler;
	dev->handler_ctx = handler_ctx;
	dev->ctrl_fd = -1;
	dev->cdev_fd = -1;
	dev->stop_efd = -1;
	dev->ready_fd = -1;
	dev->async_enabled = 0;  /* Will be set based on device flags after get_dev_info */

	/* Open control device */
	dev->ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (dev->ctrl_fd < 0) {
		ret = -errno;
		erofs_err("Failed to open %s: %s", UBLK_CTRL_DEV, strerror(errno));
		goto err_free;
	}

	/* Initialize persistent control ring to avoid per-command ring creation */
	ret = ublk_ctrl_ring_init(&dev->ctrl_ring);
	if (ret < 0) {
		erofs_err("ctrl ring init failed: %s", strerror(-ret));
		goto err_close_ctrl;
	}
	dev->ctrl_ring_initialized = 1;

	/* Create stop eventfd for blocking wait instead of polling */
	dev->stop_efd = eventfd(0, EFD_CLOEXEC);
	if (dev->stop_efd < 0)
		erofs_dbg("stop eventfd creation failed: %s", strerror(errno));

	/* Add device */
	ret = ublk_add_dev(dev, info);
	if (ret < 0)
		goto err_close_ctrl;

	/* Open char device */
	snprintf(cdev_path, sizeof(cdev_path), UBLK_CDEV_FMT,
		 dev->dev_info.dev_id);
	dev->cdev_fd = open(cdev_path, O_RDWR);
	if (dev->cdev_fd < 0) {
		ret = -errno;
		erofs_err("Failed to open %s: %s", cdev_path, strerror(errno));
		goto err_del_dev;
	}

	/* Set parameters */
	ret = ublk_set_params(dev, info);
	if (ret < 0)
		goto err_close_cdev;

	/* Allocate queues */
	dev->queues = calloc(dev->dev_info.nr_hw_queues,
			     sizeof(struct erofs_ublk_queue));
	if (!dev->queues) {
		ret = -ENOMEM;
		goto err_close_cdev;
	}

	/* Initialize queues */
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++) {
		ret = ublk_init_queue(dev, i);
		if (ret < 0)
			goto err_cleanup_queues;
	}

	*pdev = dev;
	return 0;

err_cleanup_queues:
	for (i = i - 1; i >= 0; i--)
		ublk_cleanup_queue(dev, i);
	free(dev->queues);
err_close_cdev:
	close(dev->cdev_fd);
err_del_dev:
	ublk_del_dev(dev);
err_close_ctrl:
	close(dev->ctrl_fd);
err_free:
	free(dev);
	return ret;
#endif
}

int erofs_ublk_start(struct erofs_ublk_dev *dev)
{
#ifndef HAVE_LIBURING
	(void)dev;
	return -EOPNOTSUPP;
#else
	pthread_barrier_t init_barrier;
	int i, ret;

	if (!dev)
		return -EINVAL;

	/* Apply OOM protection before starting IO threads */
	ublk_apply_oom_protection();

	/* Initialize barrier: nr_hw_queues threads + 1 main thread */
	ret = pthread_barrier_init(&init_barrier,
				   NULL, dev->dev_info.nr_hw_queues + 1);
	if (ret) {
		erofs_err("pthread_barrier_init failed: %s", strerror(ret));
		return -ret;
	}

	/* Start queue threads */
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++) {
		dev->queues[i].init_barrier = &init_barrier;
		ret = pthread_create(&dev->queues[i].thread, NULL,
				     ublk_queue_thread, &dev->queues[i]);
		if (ret) {
			erofs_err("pthread_create failed: %s", strerror(ret));
			goto err_stop_threads;
		}
	}

	/* Wait for all queue threads to finish initialization */
	pthread_barrier_wait(&init_barrier);
	pthread_barrier_destroy(&init_barrier);
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++)
		dev->queues[i].init_barrier = NULL;

	/* Start the device */
	ret = ublk_start_dev(dev);
	if (ret < 0)
		goto err_stop_threads;

	dev->running = 1;

	/* Signal readiness to parent process via ready_fd */
	if (dev->ready_fd >= 0) {
		char ready = 0;
		if (write(dev->ready_fd, &ready, 1) != 1)
			erofs_dbg("ready_fd write failed");
		close(dev->ready_fd);
		dev->ready_fd = -1;
	}
	erofs_info("ublk device started successfully");

	/* Wait for stop signal via eventfd (blocking, no polling) */
	if (dev->stop_efd >= 0) {
		uint64_t val;
		while (!dev->stop_requested) {
			if (read(dev->stop_efd, &val, sizeof(val)) < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			break;
		}
	} else {
		/* Fallback: polling if eventfd not available */
		while (!dev->stop_requested)
			usleep(100000);
	}

	return 0;

err_stop_threads:
	pthread_barrier_destroy(&init_barrier);
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++) {
		dev->queues[i].init_barrier = NULL;
		if (dev->queues[i].thread) {
			dev->queues[i].state |= UBLKSRV_QUEUE_STOPPING;
			pthread_join(dev->queues[i].thread, NULL);
			dev->queues[i].thread = 0;
		}
	}
	return ret;
#endif
}

int erofs_ublk_stop(struct erofs_ublk_dev *dev)
{
#ifndef HAVE_LIBURING
	(void)dev;
	return -EOPNOTSUPP;
#else
	int i;

	if (!dev)
		return -EINVAL;

	dev->stop_requested = 1;

	/* Wake up the main thread waiting on stop_efd */
	if (dev->stop_efd >= 0) {
		uint64_t val = 1;
		if (write(dev->stop_efd, &val, sizeof(val)) != sizeof(val))
			erofs_dbg("stop_efd write failed");
	}

	/* Stop all queues */
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++)
		dev->queues[i].state |= UBLKSRV_QUEUE_STOPPING;

	/* Stop device */
	if (dev->running)
		ublk_stop_dev(dev);

	dev->running = 0;
	return 0;
#endif
}

void erofs_ublk_set_ready_fd(struct erofs_ublk_dev *dev, int fd)
{
	if (dev)
		dev->ready_fd = fd;
}

void erofs_ublk_destroy(struct erofs_ublk_dev *dev)
{
#ifdef HAVE_LIBURING
	int i;

	if (!dev)
		return;

	erofs_ublk_stop(dev);

	/* Cleanup queues */
	if (dev->queues) {
		for (i = 0; i < dev->dev_info.nr_hw_queues; i++)
			ublk_cleanup_queue(dev, i);
		free(dev->queues);
	}

	/* Close char device */
	if (dev->cdev_fd >= 0)
		close(dev->cdev_fd);

	/* Delete and close control device */
	if (dev->ctrl_fd >= 0) {
		ublk_del_dev(dev);
		close(dev->ctrl_fd);
	}

	/* Cleanup persistent control ring */
	if (dev->ctrl_ring_initialized)
		io_uring_queue_exit(&dev->ctrl_ring);

	/* Cleanup stop eventfd */
	if (dev->stop_efd >= 0)
		close(dev->stop_efd);

	free(dev);
#endif
}

int erofs_ublk_get_dev_id(struct erofs_ublk_dev *dev)
{
	if (!dev)
		return -EINVAL;
	return dev->dev_info.dev_id;
}

int erofs_ublk_get_dev_path(struct erofs_ublk_dev *dev, char *buf, size_t buflen)
{
	if (!dev || !buf || buflen == 0)
		return -EINVAL;

	snprintf(buf, buflen, UBLK_BDEV_FMT, dev->dev_info.dev_id);
	return 0;
}

const char *erofs_ublk_op_name(int op)
{
	static const char *names[] = {
		[UBLK_IO_OP_READ] = "READ",
		[UBLK_IO_OP_WRITE] = "WRITE",
		[UBLK_IO_OP_FLUSH] = "FLUSH",
		[UBLK_IO_OP_DISCARD] = "DISCARD",
		[UBLK_IO_OP_WRITE_ZEROES] = "WRITE_ZEROES",
	};

	if (op >= 0 && op < (int)(sizeof(names)/sizeof(names[0])) && names[op])
		return names[op];
	return "UNKNOWN";
}

/*
 * Signal handler for graceful shutdown
 */
static void ublk_sig_handler(int sig)
{
	struct erofs_ublk_dev *dev = (struct erofs_ublk_dev *)g_sig_dev;

	if (sig == SIGTERM || sig == SIGINT) {
		erofs_info("received signal %d, stopping ublk device", sig);
		if (dev)
			erofs_ublk_stop(dev);
	}
}

int erofs_ublk_set_sig_handler(struct erofs_ublk_dev *dev)
{
#ifndef HAVE_LIBURING
	(void)dev;
	return -EOPNOTSUPP;
#else
	struct sigaction sa;

	if (!dev)
		return -EINVAL;

	g_sig_dev = dev;

	memset(&sa, 0, sizeof(sa));
	sa.sa_handler = ublk_sig_handler;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;

	if (sigaction(SIGTERM, &sa, NULL) < 0) {
		erofs_err("sigaction(SIGTERM) failed: %s", strerror(errno));
		return -errno;
	}

	if (sigaction(SIGINT, &sa, NULL) < 0) {
		erofs_err("sigaction(SIGINT) failed: %s", strerror(errno));
		return -errno;
	}

	erofs_dbg("signal handlers installed for SIGTERM and SIGINT");
	return 0;
#endif
}

int erofs_ublk_recover_dev(int dev_id,
			   erofs_ublk_io_handler_t handler,
			   void *handler_ctx,
			   struct erofs_ublk_dev **pdev)
{
#ifndef HAVE_LIBURING
	(void)dev_id;
	(void)handler;
	(void)handler_ctx;
	(void)pdev;
	return -EOPNOTSUPP;
#else
	struct erofs_ublk_dev *dev;
	char cdev_path[64];
	int i, ret;

	if (dev_id < 0 || !pdev)
		return -EINVAL;

	dev = calloc(1, sizeof(*dev));
	if (!dev)
		return -ENOMEM;

	dev->handler = handler;
	dev->handler_ctx = handler_ctx;
	dev->ctrl_fd = -1;
	dev->cdev_fd = -1;
	dev->stop_efd = -1;
	dev->ready_fd = -1;
	dev->async_enabled = 0;

	/* Open control device */
	dev->ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (dev->ctrl_fd < 0) {
		ret = -errno;
		erofs_err("Failed to open %s: %s", UBLK_CTRL_DEV,
			  strerror(errno));
		goto err_free;
	}

	/* Initialize persistent control ring */
	ret = ublk_ctrl_ring_init(&dev->ctrl_ring);
	if (ret < 0) {
		erofs_err("ctrl ring init failed: %s", strerror(-ret));
		goto err_close_ctrl;
	}
	dev->ctrl_ring_initialized = 1;

	/* Create stop eventfd */
	dev->stop_efd = eventfd(0, EFD_CLOEXEC);
	if (dev->stop_efd < 0)
		erofs_dbg("stop eventfd creation failed: %s",
			  strerror(errno));

	/* Get existing device info */
	ret = ublk_get_dev_info(dev, dev_id);
	if (ret < 0)
		goto err_close_ctrl;

	/* Check if device supports recovery */
	if (!(dev->dev_info.flags & UBLK_F_USER_RECOVERY)) {
		erofs_err("Device %d does not support user recovery", dev_id);
		ret = -EOPNOTSUPP;
		goto err_close_ctrl;
	}

	/* Check if device is quiesced (ready for recovery) */
	if (dev->dev_info.state != UBLK_S_DEV_QUIESCED &&
	    dev->dev_info.state != UBLK_S_DEV_FAIL_IO) {
		erofs_err("Device %d is not in recoverable state (state=%d)",
			  dev_id, dev->dev_info.state);
		ret = -EBUSY;
		goto err_close_ctrl;
	}

	/* Get device parameters before starting recovery */
	ret = ublk_get_params(dev);
	if (ret < 0)
		goto err_close_ctrl;

	/* Start recovery process */
	ret = ublk_start_recovery(dev);
	if (ret < 0)
		goto err_close_ctrl;

	/* Open char device */
	snprintf(cdev_path, sizeof(cdev_path), UBLK_CDEV_FMT, dev_id);
	dev->cdev_fd = open(cdev_path, O_RDWR);
	if (dev->cdev_fd < 0) {
		ret = -errno;
		erofs_err("Failed to open %s: %s", cdev_path, strerror(errno));
		goto err_close_ctrl;
	}

	/* Allocate queues */
	dev->queues = calloc(dev->dev_info.nr_hw_queues,
			     sizeof(struct erofs_ublk_queue));
	if (!dev->queues) {
		ret = -ENOMEM;
		goto err_close_cdev;
	}

	/* Initialize queues */
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++) {
		ret = ublk_init_queue(dev, i);
		if (ret < 0)
			goto err_cleanup_queues;
	}

	*pdev = dev;
	return 0;

err_cleanup_queues:
	for (i = i - 1; i >= 0; i--)
		ublk_cleanup_queue(dev, i);
	free(dev->queues);
err_close_cdev:
	close(dev->cdev_fd);
err_close_ctrl:
	close(dev->ctrl_fd);
err_free:
	free(dev);
	return ret;
#endif
}

int erofs_ublk_complete_recovery(struct erofs_ublk_dev *dev)
{
#ifndef HAVE_LIBURING
	(void)dev;
	return -EOPNOTSUPP;
#else
	pthread_barrier_t init_barrier;
	int i, ret;

	if (!dev)
		return -EINVAL;

	/* Apply OOM protection */
	if (!(dev->dev_info.flags & UBLK_F_UNPRIVILEGED_DEV))
		ublk_apply_oom_protection();

	/* Initialize barrier: nr_hw_queues threads + 1 main thread */
	ret = pthread_barrier_init(&init_barrier,
				   NULL, dev->dev_info.nr_hw_queues + 1);
	if (ret) {
		erofs_err("pthread_barrier_init failed: %s", strerror(ret));
		return -ret;
	}

	/* Start queue threads */
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++) {
		dev->queues[i].init_barrier = &init_barrier;
		ret = pthread_create(&dev->queues[i].thread, NULL,
				     ublk_queue_thread, &dev->queues[i]);
		if (ret) {
			erofs_err("pthread_create failed: %s", strerror(ret));
			goto err_stop_threads;
		}
	}

	/* Wait for all queue threads to finish initialization */
	pthread_barrier_wait(&init_barrier);
	pthread_barrier_destroy(&init_barrier);
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++)
		dev->queues[i].init_barrier = NULL;

	/* Complete recovery */
	ret = ublk_end_recovery(dev);
	if (ret < 0)
		goto err_stop_threads;

	dev->running = 1;
	erofs_info("ublk device recovery completed successfully");

	/* Wait for stop signal via eventfd (blocking, no polling) */
	if (dev->stop_efd >= 0) {
		uint64_t val;

		while (!dev->stop_requested) {
			if (read(dev->stop_efd, &val, sizeof(val)) < 0) {
				if (errno == EINTR)
					continue;
				break;
			}
			break;
		}
	} else {
		while (!dev->stop_requested)
			usleep(100000);
	}

	return 0;

err_stop_threads:
	pthread_barrier_destroy(&init_barrier);
	for (i = 0; i < dev->dev_info.nr_hw_queues; i++) {
		dev->queues[i].init_barrier = NULL;
		if (dev->queues[i].thread) {
			dev->queues[i].state |= UBLKSRV_QUEUE_STOPPING;
			pthread_join(dev->queues[i].thread, NULL);
			dev->queues[i].thread = 0;
		}
	}
	return ret;
#endif
}

int erofs_ublk_is_recoverable(int dev_id)
{
#ifndef HAVE_LIBURING
	(void)dev_id;
	return 0;
#else
	struct erofs_ublk_dev dev;
	int ctrl_fd, ret;

	ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (ctrl_fd < 0)
		return 0;

	memset(&dev, 0, sizeof(dev));
	dev.ctrl_fd = ctrl_fd;

	ret = ublk_get_dev_info(&dev, dev_id);
	close(ctrl_fd);

	if (ret < 0)
		return 0;

	/* Check if device supports recovery and is quiesced */
	if ((dev.dev_info.flags & UBLK_F_USER_RECOVERY) &&
	    dev.dev_info.state == UBLK_S_DEV_QUIESCED)
		return 1;

	return 0;
#endif
}

/*
 * Async IO API implementations
 */

int erofs_ublk_aio_get_ctx(struct erofs_ublk_dev *dev,
			   const struct erofs_ublk_request *req,
			   struct erofs_ublk_aio_ctx **ctx)
{
#ifndef HAVE_LIBURING
	(void)dev;
	(void)req;
	(void)ctx;
	return -EOPNOTSUPP;
#else
	struct erofs_ublk_aio_ctx *actx;

	if (!dev || !req || !ctx)
		return -EINVAL;

	if (!dev->async_enabled)
		return -EINVAL;

	actx = calloc(1, sizeof(*actx));
	if (!actx)
		return -ENOMEM;

	actx->q_id = req->q_id;
	actx->tag = req->tag;
	actx->dev = dev;
	actx->priv = NULL;

	*ctx = actx;
	return 0;
#endif
}

int erofs_ublk_aio_complete(struct erofs_ublk_aio_ctx *ctx, int result)
{
#ifndef HAVE_LIBURING
	(void)ctx;
	(void)result;
	return -EOPNOTSUPP;
#else
	struct erofs_ublk_dev *dev;
	struct erofs_ublk_queue *q;
	struct erofs_ublk_io *io;
	const struct ublksrv_io_desc *iod;
	int ret;

	if (!ctx || !ctx->dev)
		return -EINVAL;

	dev = ctx->dev;
	if (ctx->q_id >= dev->dev_info.nr_hw_queues) {
		free(ctx);
		return -EINVAL;
	}

	q = &dev->queues[ctx->q_id];
	if (ctx->tag >= q->q_depth) {
		free(ctx);
		return -EINVAL;
	}

	io = &q->ios[ctx->tag];
	iod = &q->io_cmd_buf[ctx->tag];

	/* Verify this IO is pending async completion */
	if (!(io->flags & UBLKSRV_IO_ASYNC_PENDING)) {
		erofs_err("aio_complete: tag %d not pending", ctx->tag);
		free(ctx);
		return -EINVAL;
	}

	if (result < 0) {
		io->result = result;
	} else {
		size_t io_bytes = (size_t)iod->nr_sectors << 9;
		__u8 op = ublksrv_get_op(iod);

		/*
		 * For READ operations in USER_COPY mode, copy data to kernel
		 */
		if (op == UBLK_IO_OP_READ && io_bytes > 0) {
			ret = ublk_user_copy_write(q, ctx->tag, io_bytes);
			if (ret < 0) {
				io->result = ret;
			} else {
				io->result = io_bytes;
			}
		} else {
			io->result = result > 0 ? result : io_bytes;
		}
	}

	/* Mark as ready for commit */
	io->flags = UBLKSRV_NEED_COMMIT_RQ_COMP;

	/* Queue the completion command */
	ret = ublk_queue_io_cmd(q, ctx->tag);
	if (ret == 0) {
		/* Submit immediately */
		io_uring_submit(&q->ring);
	}

	free(ctx);
	return ret;
#endif
}

void *erofs_ublk_aio_get_buf(struct erofs_ublk_aio_ctx *ctx)
{
#ifndef HAVE_LIBURING
	(void)ctx;
	return NULL;
#else
	struct erofs_ublk_dev *dev;
	struct erofs_ublk_queue *q;

	if (!ctx || !ctx->dev)
		return NULL;

	dev = ctx->dev;
	if (ctx->q_id >= dev->dev_info.nr_hw_queues)
		return NULL;

	q = &dev->queues[ctx->q_id];
	if (ctx->tag >= q->q_depth)
		return NULL;

	return ublk_get_io_buf(q, ctx->tag);
#endif
}

int erofs_ublk_del_dev_by_id(int dev_id)
{
#ifndef HAVE_LIBURING
	(void)dev_id;
	return -EOPNOTSUPP;
#else
	struct ublksrv_ctrl_cmd cmd = {0};
	int ctrl_fd, ret;

	ctrl_fd = open(UBLK_CTRL_DEV, O_RDWR);
	if (ctrl_fd < 0)
		return -errno;

	cmd.dev_id = dev_id;
	cmd.queue_id = (__u16)-1;

	ret = ublk_ctrl_cmd(ctrl_fd, UBLK_U_CMD_STOP_DEV, &cmd);
	if (ret < 0 && ret != -ENODEV)
		erofs_dbg("STOP_DEV %d: %s", dev_id, strerror(-ret));

	ret = ublk_ctrl_cmd(ctrl_fd, UBLK_U_CMD_DEL_DEV_ASYNC, &cmd);
	close(ctrl_fd);
	return ret;
#endif
}
