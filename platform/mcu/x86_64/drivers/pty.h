/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef PTY_H
#define PTY_H

#include "interfaces/chardev.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Character device backed by a Linux pseudo-terminal. The slave side path is
 * printed on the console at initialisation and, when the OPENRTX_HOST_PTY
 * environment variable is set, a symbolic link pointing to it is created at
 * that path so that host software can use a stable name.
 */

struct ptyState {
    int fd; /**< Master side file descriptor */
};

extern const struct chardevApi pty_chardev_api;

/**
 * Instantiate a pseudo-terminal character device.
 *
 * @param name: device variable name.
 */
#define PTY_CHARDEV_DEFINE(name)                           \
    static struct ptyState name##_state = { .fd = -1 };    \
    const struct chardev name = { .api = &pty_chardev_api, \
                                  .config = NULL,          \
                                  .priv = &name##_state };

#ifdef __cplusplus
}
#endif

#endif /* PTY_H */
