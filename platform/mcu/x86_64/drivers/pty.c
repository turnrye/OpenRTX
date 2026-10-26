/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#define _GNU_SOURCE
#include "pty.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

static int pty_init(const struct chardev *dev)
{
    struct ptyState *st = (struct ptyState *)dev->priv;

    if (st->fd >= 0)
        return 0;

    int fd = posix_openpt(O_RDWR | O_NOCTTY | O_NONBLOCK);
    if (fd < 0)
        return -errno;

    if ((grantpt(fd) < 0) || (unlockpt(fd) < 0)) {
        close(fd);
        return -errno;
    }

    struct termios tio;
    if (tcgetattr(fd, &tio) == 0) {
        cfmakeraw(&tio);
        tcsetattr(fd, TCSANOW, &tio);
    }

    const char *name = ptsname(fd);
    printf("Host serial port: %s\n", name);

    const char *link = getenv("OPENRTX_HOST_PTY");
    if ((link != NULL) && (name != NULL)) {
        unlink(link);
        if (symlink(name, link) == 0)
            printf("Host serial link: %s\n", link);
    }

    st->fd = fd;

    return 0;
}

static void pty_terminate(const struct chardev *dev)
{
    struct ptyState *st = (struct ptyState *)dev->priv;

    if (st->fd < 0)
        return;

    close(st->fd);
    st->fd = -1;
}

static ssize_t pty_read(const struct chardev *dev, void *buf, size_t len)
{
    struct ptyState *st = (struct ptyState *)dev->priv;

    if (st->fd < 0)
        return -EBADF;

    ssize_t ret = read(st->fd, buf, len);
    if (ret < 0) {
        // No data, or no slave side attached: both mean "nothing to read".
        if ((errno == EAGAIN) || (errno == EIO))
            return 0;

        return -errno;
    }

    return ret;
}

static ssize_t pty_write(const struct chardev *dev, const void *buf, size_t len)
{
    struct ptyState *st = (struct ptyState *)dev->priv;

    if (st->fd < 0)
        return -EBADF;

    ssize_t ret = write(st->fd, buf, len);
    if (ret < 0)
        return -errno;

    return ret;
}

const struct chardevApi pty_chardev_api = {
    .init = pty_init,
    .terminate = pty_terminate,
    .read = pty_read,
    .write = pty_write,
};
