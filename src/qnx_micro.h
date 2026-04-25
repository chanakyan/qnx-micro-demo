/* SPDX-License-Identifier: BSD-2-Clause
 * Copyright (c) 2026 Rajeshkumar Venugopal / Third Buyer Advisory
 *
 * qnx_micro.h — stable C ABI for the qnx-micro microkernel
 *
 * All types are C-compatible int32_t handles.
 * Return < 0 on error (negated KernelError ordinal).
 * No C++ types leak through this header.
 */

#ifndef QNX_MICRO_H
#define QNX_MICRO_H

#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ─── Handle types ─────────────────────────────────────────────────────── */

typedef int32_t qnx_thread_id;      /**< Unique thread identifier */
typedef int32_t qnx_channel_id;     /**< Server-side IPC channel handle */
typedef int32_t qnx_connection_id;  /**< Client-side connection to a channel */
typedef int32_t qnx_process_id;     /**< Unique process identifier */
typedef int32_t qnx_capability_id;  /**< Memory capability token */

/** @brief Asynchronous pulse: non-blocking notification with code + value. */
typedef struct {
    int32_t code;   /**< Application-defined pulse type code */
    int32_t value;  /**< Application-defined pulse payload */
} qnx_pulse_t;

/* ─── Permissions ──────────────────────────────────────────────────────── */

#define QNX_PERM_NONE  0  /**< No access */
#define QNX_PERM_READ  1  /**< Read access */
#define QNX_PERM_WRITE 2  /**< Write access */
#define QNX_PERM_EXEC  4  /**< Execute access */

/* ─── Kernel lifecycle ─────────────────────────────────────────────────── */

/** @brief Initialize the kernel: create idle thread and begin scheduling. */
void    qnx_kernel_init(void);

/** @brief Advance the scheduler by one timer tick (preemption check). */
void    qnx_kernel_tick(void);

/* ─── IPC ──────────────────────────────────────────────────────────────── */

/**
 * @brief Create a new IPC channel owned by the given process.
 * @param owner Process that will receive messages on this channel.
 * @return Channel ID on success, negative error code on failure.
 */
int32_t qnx_channel_create(qnx_process_id owner);

/**
 * @brief Attach a client connection to an existing channel.
 * @param ch Target channel.
 * @param pid Client process.
 * @return Connection ID on success, negative error code on failure.
 */
int32_t qnx_connect_attach(qnx_channel_id ch, qnx_process_id pid);

/**
 * @brief Send a synchronous message (blocking) and receive the reply.
 * @param conn Connection to send over.
 * @param sbuf Send buffer.
 * @param slen Send buffer length in bytes.
 * @param rbuf Reply buffer (filled on return).
 * @param rlen Reply buffer capacity in bytes.
 * @return 0 on success, negative error code on failure.
 */
int32_t qnx_msg_send(qnx_connection_id conn,
                      const void* sbuf, size_t slen,
                      void* rbuf, size_t rlen);

/**
 * @brief Receive the next message from a channel (blocking).
 * @param ch Channel to receive from.
 * @param buf Buffer to store the incoming message.
 * @param len Buffer capacity in bytes.
 * @return Sender thread ID on success, negative error code on failure.
 */
int32_t qnx_msg_receive(qnx_channel_id ch, void* buf, size_t len);

/**
 * @brief Reply to a previously received message, unblocking the sender.
 * @param sender Thread that sent the original message.
 * @param buf Reply data buffer.
 * @param len Reply data length in bytes.
 * @return 0 on success, negative error code on failure.
 */
int32_t qnx_msg_reply(qnx_thread_id sender,
                       const void* buf, size_t len);

/**
 * @brief Send an asynchronous pulse (non-blocking).
 * @param conn Connection to send the pulse over.
 * @param code Pulse type code.
 * @param value Pulse payload value.
 * @return 0 on success, negative error code on failure.
 */
int32_t qnx_msg_send_pulse(qnx_connection_id conn,
                            int32_t code, int32_t value);

/* ─── Scheduler ────────────────────────────────────────────────────────── */

/**
 * @brief Create a new thread in a process.
 * @param pid Owning process.
 * @param priority Scheduling priority (0-255).
 * @return Thread ID on success, negative error code on failure.
 */
int32_t qnx_thread_create(qnx_process_id pid, int32_t priority);

/**
 * @brief Terminate a thread.
 * @param tid Thread to destroy.
 * @return 0 on success, negative error code on failure.
 */
int32_t qnx_thread_destroy(qnx_thread_id tid);

/* ─── Memory ───────────────────────────────────────────────────────────── */

/**
 * @brief Map a new memory region with the given permissions.
 * @param pid Process requesting the mapping.
 * @param length Region size in bytes.
 * @param perm Permission flags (QNX_PERM_* bitmask).
 * @return Base address on success (as int64_t), negative error code on failure.
 */
int64_t qnx_mmap(qnx_process_id pid, size_t length, int32_t perm);

/**
 * @brief Unmap a previously mapped memory region.
 * @param pid Process that owns the mapping.
 * @param base Virtual base address to unmap.
 * @return 0 on success, negative error code on failure.
 */
int32_t qnx_munmap(qnx_process_id pid, int64_t base);

/* ─── Resource managers ────────────────────────────────────────────────── */

/**
 * @brief Register a resource manager at the given pathname.
 * @param path Pathname prefix to claim (e.g. "/dev/ser1").
 * @param ch IPC channel where the manager receives messages.
 * @param pid Process hosting the resource manager.
 * @return 0 on success, negative error code on failure.
 */
int32_t qnx_register_resource(const char* path, qnx_channel_id ch,
                               qnx_process_id pid);

/**
 * @brief Open a pathname: resolve to a resource manager and create a connection.
 * @param path Pathname to resolve.
 * @param client_pid Process performing the open.
 * @return Connection ID on success, negative error code on failure.
 */
int32_t qnx_resolve_path(const char* path, qnx_process_id client_pid);

#ifdef __cplusplus
}
#endif

#endif /* QNX_MICRO_H */
