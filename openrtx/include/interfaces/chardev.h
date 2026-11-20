/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef CHARDEV_H
#define CHARDEV_H

#include <stddef.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Operations of a byte stream device, such as a USB virtual COM port, a UART
 * or a pseudo-terminal.
 */
struct chardev_ops {
    /**
     * Initialise the device. NULL for a device the platform already
     * initialised.
     *
     * @return 0 on success, a negative error code otherwise.
     */
    int (*init)(void);

    /**
     * Read the bytes received so far, without blocking.
     *
     * @param buf: destination buffer.
     * @param len: buffer size.
     * @return number of bytes read, a negative error code on failure.
     */
    ssize_t (*read)(void *buf, size_t len);

    /**
     * Write bytes, blocking until they are queued for transmission.
     *
     * @param buf: data to write.
     * @param len: number of bytes to write.
     * @return number of bytes written, a negative error code on failure.
     */
    ssize_t (*write)(const void *buf, size_t len);
};

#ifdef __cplusplus
}
#endif

#endif // CHARDEV_H
