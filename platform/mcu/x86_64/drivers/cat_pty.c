/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * CAT for the Linux target, on a pseudo-terminal that host tools open like a
 * radio's serial port. The path of its slave side is printed at startup and,
 * when the CAT_PTY environment variable is set, symlinked there.
 */

#define _GNU_SOURCE
#include "hwconfig.h"
#include "interfaces/chardev.h"
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

// A host that stops reading drops answers after 100 ms, not the main thread
#define WRITE_WAIT_US 100
#define WRITE_WAITS 1000

static int master = -1;

/**
 * Report a failure of the pseudo-terminal.
 *
 * @param what: what failed.
 * @return negative errno code of the failure.
 */
static int failure(const char *what)
{
    int err = errno;

    printf("CAT: %s failed: %s\n", what, strerror(err));
    return -err;
}

static int ptyInit(void)
{
    master = posix_openpt(O_RDWR | O_NOCTTY);
    if (master < 0)
        return failure("posix_openpt");

    if ((grantpt(master) < 0) || (unlockpt(master) < 0))
        return failure("grantpt");

    const char *path = ptsname(master);
    if (path == NULL)
        return failure("ptsname");

    // Keep the slave open, so the master does not fail with EIO while no
    // host is attached, and make it raw so commands pass unchanged
    int slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (slave < 0)
        return failure("open");

    struct termios tio;
    tcgetattr(slave, &tio);
    cfmakeraw(&tio);
    tcsetattr(slave, TCSANOW, &tio);

    fcntl(master, F_SETFL, fcntl(master, F_GETFL) | O_NONBLOCK);
    printf("CAT: %s\n", path);

    const char *link = getenv("CAT_PTY");
    if (link != NULL) {
        unlink(link);
        if (symlink(path, link) < 0)
            return failure("symlink");

        printf("CAT: %s -> %s\n", link, path);
    }

    return 0;
}

static ssize_t ptyRead(void *buf, size_t len)
{
    ssize_t ret = read(master, buf, len);
    bool noData = (ret < 0) && ((errno == EAGAIN) || (errno == EINTR));

    if (noData)
        return 0;
    if (ret < 0)
        return -errno;

    return ret;
}

static ssize_t ptyWrite(const void *buf, size_t len)
{
    const char *data = buf;
    size_t done = 0;
    unsigned int waits = 0;

    while (done < len) {
        ssize_t ret = write(master, &data[done], len - done);
        bool full = (ret < 0) && ((errno == EAGAIN) || (errno == EINTR));

        // Full: wait for the host to read, but not forever
        if (full) {
            if (waits++ == WRITE_WAITS)
                return -ETIMEDOUT;

            usleep(WRITE_WAIT_US);
            continue;
        }
        if (ret < 0)
            return -errno;

        done += ret;
    }

    return done;
}

const struct chardev_ops cat_chardev = {
    .init = ptyInit,
    .read = ptyRead,
    .write = ptyWrite,
};
