/* SPDX-License-Identifier: GPL-2.0+ OR Apache-2.0 */
/*
 * Copyright (C) 2026 Tencent, Inc.
 *             http://www.tencent.com/
 */
#ifndef __LIBEROFS_UBLK_H
#define __LIBEROFS_UBLK_H

#ifdef __cplusplus
extern "C" {
#endif

#include <stddef.h>
#include <stdint.h>

/* Forward declaration */
struct erofs_ublk_dev;

/* Default configuration values */
#define EROFS_UBLK_DEF_NR_HW_QUEUES	1
#define EROFS_UBLK_DEF_QUEUE_DEPTH	128
#define EROFS_UBLK_DEF_MAX_IO_BUF_BYTES	(512 * 1024)
#define EROFS_UBLK_DEF_BLK_BITS		12	/* 4KB */

/* Feature flags for erofs_ublk_dev_info */
#define EROFS_UBLK_F_UNPRIVILEGED	(1U << 0)  /* Allow non-root access */
#define EROFS_UBLK_F_USER_RECOVERY	(1U << 1)  /* Enable crash recovery */
#define EROFS_UBLK_F_USER_COPY		(1U << 2)  /* User handles data copy */
#define EROFS_UBLK_F_ASYNC		(1U << 3)  /* Enable async IO support */

/* IO operation types (matches kernel definitions) */
#define EROFS_UBLK_OP_READ		0
#define EROFS_UBLK_OP_WRITE		1
#define EROFS_UBLK_OP_FLUSH		2
#define EROFS_UBLK_OP_DISCARD		3
#define EROFS_UBLK_OP_WRITE_ZEROES	5

/* IO flags */
#define EROFS_UBLK_IO_F_FUA		(1U << 13)
#define EROFS_UBLK_IO_F_NOUNMAP		(1U << 15)

/**
 * struct erofs_ublk_dev_info - Device creation parameters
 * @nr_hw_queues: Number of hardware queues (default: 1)
 *                More queues can improve parallelism on multi-core systems.
 * @queue_depth:  Queue depth per queue (default: 128)
 *                Larger depth allows more concurrent IOs.
 * @max_io_buf_bytes: Maximum IO buffer size (default: 512KB)
 * @dev_id:       Device ID (-1 for auto-allocation)
 * @dev_size:     Total device size in bytes
 * @blkbits:      Block size in bits (default: 12 for 4KB)
 * @flags:        Feature flags (EROFS_UBLK_F_*)
 */
struct erofs_ublk_dev_info {
	uint16_t nr_hw_queues;
	uint16_t queue_depth;
	uint32_t max_io_buf_bytes;
	uint32_t dev_id;
	uint64_t dev_size;
	uint8_t blkbits;
	uint8_t reserved[3];
	uint32_t flags;
};

/**
 * struct erofs_ublk_request - IO request context passed to handler
 * @q_id:         Queue ID this request came from
 * @tag:          Request tag (unique within queue)
 * @op:           Operation type (EROFS_UBLK_OP_*)
 * @flags:        IO flags (EROFS_UBLK_IO_F_*)
 * @start_sector: Starting sector (512-byte units)
 * @nr_sectors:   Number of sectors
 * @buf:          Data buffer (pre-allocated, size = nr_sectors * 512)
 * @result:       Result code to be set by handler
 *
 * For READ operations:
 *   - Fill @buf with requested data
 *   - Set @result to bytes read, or negative errno on error
 *
 * For other operations:
 *   - Set @result to 0 on success, negative errno on error
 */
struct erofs_ublk_request {
	uint16_t q_id;
	uint16_t tag;
	uint8_t op;
	uint8_t reserved;
	uint16_t flags;
	uint64_t start_sector;
	uint32_t nr_sectors;
	uint32_t reserved2;
	void *buf;
	int result;
};

/**
 * IO request handler callback type
 * @ctx:  User-provided context
 * @req:  Request to process
 *
 * The handler is called from the queue thread context.
 * It should process the request synchronously and set req->result.
 *
 * Return: 0 on success (req->result contains the actual result),
 *         negative errno on fatal error that should stop the device.
 *
 * Note: For EROFS, typically only READ operations need to be handled.
 *       Other operations can return -EOPNOTSUPP.
 */
typedef int (*erofs_ublk_io_handler_t)(void *ctx, struct erofs_ublk_request *req);

/**
 * erofs_ublk_init - Initialize ublk subsystem
 *
 * This should be called once before using any other ublk functions.
 *
 * Return: 0 on success, -EOPNOTSUPP if ublk is not supported.
 */
int erofs_ublk_init(void);

/**
 * erofs_ublk_exit - Cleanup ublk subsystem
 *
 * Call this when done with all ublk operations.
 */
void erofs_ublk_exit(void);

/**
 * erofs_ublk_is_supported - Check if ublk is supported on this system
 *
 * Checks for:
 *   - liburing availability (compile-time)
 *   - /dev/ublk-control existence (runtime)
 *
 * Return: 1 if supported, 0 otherwise.
 */
int erofs_ublk_is_supported(void);

/**
 * erofs_ublk_devscan - Scan for next available device ID
 *
 * Finds the lowest available ublk device ID by checking for
 * non-existent /dev/ublkcN character devices.
 *
 * Return: Available device ID (>= 0), or negative errno on failure.
 */
int erofs_ublk_devscan(void);

/**
 * erofs_ublk_create_dev - Create a new ublk device
 * @info:        Device parameters (NULL for defaults)
 * @handler:     IO request handler callback (required)
 * @handler_ctx: Context passed to handler
 * @pdev:        Output device handle
 *
 * Creates and configures a ublk device. After creation, the device
 * exists but is not yet active. Call erofs_ublk_start() to activate.
 *
 * The device will appear as /dev/ublkbN after start.
 *
 * Return: 0 on success, negative errno on failure.
 */
int erofs_ublk_create_dev(const struct erofs_ublk_dev_info *info,
			  erofs_ublk_io_handler_t handler,
			  void *handler_ctx,
			  struct erofs_ublk_dev **pdev);

/**
 * erofs_ublk_start - Start the ublk device (blocking)
 * @dev: Device handle
 *
 * Starts the device and begins processing IO requests.
 * This function blocks until erofs_ublk_stop() is called
 * from another thread or signal handler.
 *
 * Once started, the block device /dev/ublkbN becomes accessible.
 * If a ready_fd was set via erofs_ublk_set_ready_fd(), the device ID
 * will be written to it after the device is started.
 *
 * Return: 0 on normal exit, negative errno on failure.
 */
int erofs_ublk_start(struct erofs_ublk_dev *dev);

/**
 * erofs_ublk_set_ready_fd - Set fd to notify when device is ready
 * @dev: Device handle
 * @fd: File descriptor to write a ready byte to after ublk_start_dev()
 *
 * When set, erofs_ublk_start() will write a single byte (0) to this fd
 * after the device is successfully started and /dev/ublkbN is available,
 * then close it. This allows the parent process to avoid polling.
 */
void erofs_ublk_set_ready_fd(struct erofs_ublk_dev *dev, int fd);

/**
 * erofs_ublk_stop - Stop the ublk device
 * @dev: Device handle
 *
 * Signals the device to stop processing IO requests.
 * This causes erofs_ublk_start() to return.
 *
 * After stop, the block device /dev/ublkbN becomes inaccessible.
 *
 * Return: 0 on success, negative errno on failure.
 */
int erofs_ublk_stop(struct erofs_ublk_dev *dev);

/**
 * erofs_ublk_set_sig_handler - Install signal handler for graceful shutdown
 * @dev: Device handle
 *
 * Installs signal handlers for SIGTERM and SIGINT that will call
 * erofs_ublk_stop() when received. This allows for graceful shutdown
 * when running as a daemon.
 *
 * Return: 0 on success, negative errno on failure.
 */
int erofs_ublk_set_sig_handler(struct erofs_ublk_dev *dev);


/*
 * Asynchronous IO Support
 *
 * For applications that need non-blocking IO handling (e.g., when data
 * comes from network or requires async decompression), the AIO API allows
 * completing requests asynchronously.
 *
 * Usage:
 *   1. Create device with EROFS_UBLK_F_ASYNC flag
 *   2. In handler, return EROFS_UBLK_IO_ASYNC to defer completion
 *   3. Later, call erofs_ublk_aio_complete() to finish the request
 */

/* Return value from handler to indicate async completion */
#define EROFS_UBLK_IO_ASYNC	1

/**
 * struct erofs_ublk_aio_ctx - Async IO context for deferred completion
 * @q_id:   Queue ID
 * @tag:    Request tag
 * @dev:    Device handle (internal use)
 *
 * This structure is passed to the async completion function.
 * It must be allocated/freed by the caller.
 */
struct erofs_ublk_aio_ctx {
	uint16_t q_id;
	uint16_t tag;
	void *dev;	/* Internal: device handle */
	void *priv;	/* User private data */
};

/**
 * erofs_ublk_aio_get_ctx - Get async context from request
 * @dev: Device handle
 * @req: Current request being processed
 * @ctx: Output async context (caller must free)
 *
 * Call this in your handler before returning EROFS_UBLK_IO_ASYNC.
 * The returned context must be passed to erofs_ublk_aio_complete().
 *
 * Return: 0 on success, negative errno on failure.
 */
int erofs_ublk_aio_get_ctx(struct erofs_ublk_dev *dev,
			   const struct erofs_ublk_request *req,
			   struct erofs_ublk_aio_ctx **ctx);

/**
 * erofs_ublk_aio_complete - Complete an async IO request
 * @ctx:    Async context from erofs_ublk_aio_get_ctx()
 * @result: Result code (bytes transferred or negative errno)
 *
 * Call this from any thread to complete a previously deferred request.
 * For READ operations, ensure the buffer is filled before calling.
 * The ctx is freed by this function.
 *
 * Return: 0 on success, negative errno on failure.
 */
int erofs_ublk_aio_complete(struct erofs_ublk_aio_ctx *ctx, int result);

/**
 * erofs_ublk_aio_get_buf - Get buffer for async request
 * @ctx: Async context
 *
 * Returns the buffer associated with the async request.
 * For READ operations, fill this buffer before calling aio_complete().
 *
 * Return: Buffer pointer, or NULL on error.
 */
void *erofs_ublk_aio_get_buf(struct erofs_ublk_aio_ctx *ctx);

/**
 * erofs_ublk_destroy - Destroy the ublk device
 * @dev: Device handle
 *
 * Stops the device if running and releases all resources.
 * The device handle becomes invalid after this call.
 *
 * This will also remove the block device from the system.
 */
void erofs_ublk_destroy(struct erofs_ublk_dev *dev);

/**
 * erofs_ublk_get_dev_id - Get the device ID
 * @dev: Device handle
 *
 * Return: Device ID (>= 0), or negative errno on failure.
 */
int erofs_ublk_get_dev_id(struct erofs_ublk_dev *dev);

/**
 * erofs_ublk_get_dev_path - Get the block device path
 * @dev:    Device handle
 * @buf:    Output buffer
 * @buflen: Buffer length
 *
 * Writes the block device path (e.g., "/dev/ublkb0") to buffer.
 *
 * Return: 0 on success, negative errno on failure.
 */
int erofs_ublk_get_dev_path(struct erofs_ublk_dev *dev, char *buf, size_t buflen);

/**
 * erofs_ublk_op_name - Get human-readable name for IO operation
 * @op: Operation type (EROFS_UBLK_OP_*)
 *
 * Return: Static string with operation name.
 */
const char *erofs_ublk_op_name(int op);

/**
 * erofs_ublk_del_dev_by_id - Delete a ublk device by its device ID
 * @dev_id: Device ID (the N in /dev/ublkbN)
 *
 * Sends UBLK_U_CMD_DEL_DEV to the kernel to remove the ublk device.
 * This will also terminate the associated ublk backend process.
 *
 * Return: 0 on success, negative errno on failure.
 */
int erofs_ublk_del_dev_by_id(int dev_id);

#ifdef __cplusplus
}
#endif

#endif /* __LIBEROFS_UBLK_H */
