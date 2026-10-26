/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 * 
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <stdio.h>
#include <reent.h>
#include "../drivers/usb_vcom.h"
#include "interfaces/delays.h"
#include "filesystem/file_access.h"

using namespace std;

#ifdef __cplusplus
extern "C" {
#endif

/**
 * \internal
 * _write_r, write to a file
 */
int _write_r(struct _reent *ptr, int fd, const void *buf, size_t cnt)
{
    #ifdef ENABLE_STDIO
    if(fd == STDOUT_FILENO || fd == STDERR_FILENO)
    {
        vcom_writeBlock(buf, cnt);
        return cnt;
    }
    #else
    (void) ptr;
    (void) fd;
    (void) buf;
    (void) cnt;
    #endif

    /* If fd is not stdout or stderr */
    ptr->_errno = EBADF;
    return -1;
}

/**
 * \internal
 * _read_r, read from a file.
 */
int _read_r(struct _reent *ptr, int fd, void *buf, size_t cnt)
{
    #ifdef ENABLE_STDIO
    if(fd == STDIN_FILENO)
    {
        // Block until the requested amount of data has been read, keeping
        // what each nonblocking read returns.
        size_t rd = 0;
        while(rd < cnt)
        {
            ssize_t r = vcom_readBlock(((uint8_t *) buf) + rd, cnt - rd);
            if(r < 0) return r;
            if(r == 0) delayMs(1);
            rd += r;
        }

        return cnt;
    }
    #else
    (void) ptr;
    (void) fd;
    (void) buf;
    (void) cnt;
    #endif

    /* If fd is not stdin */
    ptr->_errno = EBADF;
    return -1;
}

#ifdef __cplusplus
}
#endif
