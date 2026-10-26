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
 * Character device interface: a byte-oriented, transport-agnostic link to a
 * host computer (USB CDC-ACM, UART, Linux pseudo-terminal, ...).
 *
 * Reads never block: they return the bytes already available, possibly none.
 * Writes may block for the time needed to push the data out of the device.
 */

struct chardev;

struct chardevApi {
    /**
     * Initialise the device.
     *
     * @param dev: pointer to device descriptor.
     * @return zero on success, a negative error code otherwise.
     */
    int (*init)(const struct chardev *dev);

    /**
     * Shut down the device.
     *
     * @param dev: pointer to device descriptor.
     */
    void (*terminate)(const struct chardev *dev);

    /**
     * Non-blocking read.
     *
     * @param dev: pointer to device descriptor.
     * @param buf: destination buffer.
     * @param len: maximum number of bytes to read.
     * @return number of bytes read, zero when none is available, or a
     * negative error code.
     */
    ssize_t (*read)(const struct chardev *dev, void *buf, size_t len);

    /**
     * Write a block of data.
     *
     * @param dev: pointer to device descriptor.
     * @param buf: source buffer.
     * @param len: number of bytes to write.
     * @return number of bytes written or a negative error code.
     */
    ssize_t (*write)(const struct chardev *dev, const void *buf, size_t len);
};

struct chardev {
    const struct chardevApi *api; /**< Driver functions                  */
    const void *config;           /**< Driver-specific configuration     */
    void *priv;                   /**< Driver-specific runtime state     */
};

static inline int chardev_init(const struct chardev *dev)
{
    return dev->api->init(dev);
}

static inline void chardev_terminate(const struct chardev *dev)
{
    dev->api->terminate(dev);
}

static inline ssize_t chardev_read(const struct chardev *dev, void *buf,
                                   size_t len)
{
    return dev->api->read(dev, buf, len);
}

static inline ssize_t chardev_write(const struct chardev *dev, const void *buf,
                                    size_t len)
{
    return dev->api->write(dev, buf, len);
}

#ifdef __cplusplus
}
#endif

#endif /* CHARDEV_H */
