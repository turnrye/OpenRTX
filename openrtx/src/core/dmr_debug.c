/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "hwconfig.h"
#include "core/dmr_debug.h"

#if defined(CONFIG_DMR)

#include "interfaces/delays.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#if defined(PLATFORM_LINUX)
#include <unistd.h>
#elif defined(CONFIG_USB_SERIAL)
#include "interfaces/usb_serial.h"
#else
#include "drivers/usb_vcom.h"
#endif

/*
 * The DMR voice codec self-test, provided only by targets that build the
 * vocoder driver (dmr_codec). A weak reference so the tap links everywhere;
 * the 'c' command reports it unavailable where the symbol is absent.
 */
void dmr_codec_selfTest(void) __attribute__((weak));

/*
 * The transmit test pattern switch, provided by the HR_C6000 DMR driver; weak
 * for the same reason.
 */
void dmr_setPayloadMarker(const bool enable) __attribute__((weak));
static bool marker;

#define TAP_BUF_SIZE    2048    /* Message buffer size, in bytes            */
#define TAP_CHUNK_SIZE   256    /* Maximum amount of bytes sent per task run */
#define TAP_LINE_SIZE    128    /* Maximum length of a single message       */
#define TAP_MAX_RECORD    48    /* Maximum payload of a binary record       */

static char    buf[TAP_BUF_SIZE];
/*
 * Messages are formatted here rather than on the stack of whoever is logging:
 * the rtx thread has little of it to spare. Guarded by the same mutex as the
 * ring buffer below.
 */
static char    fmtBuf[TAP_LINE_SIZE];
static size_t  head;            /* Index of the first free byte             */
static size_t  tail;            /* Index of the first byte to be sent       */
static size_t  used;            /* Amount of bytes waiting to be sent       */
static size_t  dropped;         /* Bytes discarded because the buffer filled */
static uint8_t channels;        /* Bitmap of the enabled message categories */
static bool    initialised;

/*
 * Timestamps are relative to the tap initialisation: the linux target counts
 * ticks from the epoch, which would make the log unreadable and impossible to
 * compare against a radio one.
 */
static long long baseTick;

static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;

/*
 * Writes towards the host are serialized on their own lock: the buffer flush
 * and the synchronous markers run on different threads, while the USB driver
 * keeps its transfer state in a single set of variables. Two writes running
 * at once corrupt the endpoint state and can wedge the whole device.
 */
static pthread_mutex_t wireMutex = PTHREAD_MUTEX_INITIALIZER;

static const char hexDigits[] = "0123456789ABCDEF";

/**
 * Get the message timestamp, in milliseconds since the tap was initialised.
 */
static unsigned long tapTimestamp()
{
    return (unsigned long) (getTick() - baseTick);
}

/**
 * Send a block of data towards the host. On radio targets this goes to the USB
 * CDC device brought up at boot, on the linux target to standard output.
 */
static void tapWrite(const void *data, const size_t len)
{
    pthread_mutex_lock(&wireMutex);

#if defined(PLATFORM_LINUX)
    ssize_t ret = write(STDOUT_FILENO, data, len);
    (void) ret;
#elif defined(CONFIG_USB_SERIAL)
    ssize_t ret = usb_serial_write(data, len);
    (void) ret;
#else
    vcom_writeBlock(data, len);
#endif

    pthread_mutex_unlock(&wireMutex);
}

/**
 * Append a message to the buffer, discarding it as a whole if it does not fit.
 * Partial messages are never stored, so that the host always sees either a
 * complete line or nothing at all.
 */
static void tapPushLocked(const char *data, const size_t len)
{
    if(len > (TAP_BUF_SIZE - used))
    {
        dropped += len;
    }
    else
    {
        for(size_t i = 0; i < len; i++)
        {
            buf[head] = data[i];
            head = (head + 1) % TAP_BUF_SIZE;
        }

        used += len;
    }
}

/**
 * Append a message to the buffer, taking the lock around it.
 */
static void tapPush(const char *data, const size_t len)
{
    pthread_mutex_lock(&mutex);
    tapPushLocked(data, len);
    pthread_mutex_unlock(&mutex);
}

/**
 * Handle the single character commands sent by the host, allowing the message
 * categories to be turned on and off from a plain terminal emulator. The linux
 * target has no command channel, as its standard input is used elsewhere.
 */
static void tapPollCommands()
{
#if !defined(PLATFORM_LINUX)
    uint8_t cmd[8];
#if defined(CONFIG_USB_SERIAL)
    ssize_t num = usb_serial_read(cmd, sizeof(cmd));
#else
    ssize_t num = vcom_readBlock(cmd, sizeof(cmd));
#endif

    for(ssize_t i = 0; i < num; i++)
    {
        uint8_t ch = 0;

        switch(cmd[i])
        {
            case '1': ch = DMR_DBG_INFO; break;
            case '2': ch = DMR_DBG_IRQ;  break;
            case '3': ch = DMR_DBG_LC;   break;
            case '4': ch = DMR_DBG_VP;   break;

            case '?':
                dmrDebug_log(DMR_DBG_INFO, "tap: channels %02X "
                             "(1 info, 2 irq, 3 lc, 4 payload; c codec "
                             "self-test, m tx test pattern)", channels);
                continue;

            case 'm':
                if(dmr_setPayloadMarker != NULL)
                {
                    marker = !marker;
                    dmr_setPayloadMarker(marker);
                    dmrDebug_log(DMR_DBG_INFO, "tx: test pattern %s",
                                 marker ? "on" : "off");
                }
                continue;

            case 'c':
                /*
                 * Run the DMR voice codec self-test, if this target builds the
                 * vocoder driver. The reference is weak so targets without it
                 * still link.
                 */
                if(dmr_codec_selfTest != NULL)
                    dmr_codec_selfTest();
                else
                    dmrDebug_log(DMR_DBG_INFO,
                                 "codec: not available on this build");
                continue;

            default:
                continue;
        }

        dmrDebug_enableChannels(ch, (channels & ch) == 0);
        dmrDebug_log(DMR_DBG_INFO, "tap: channels %02X", channels);
    }
#endif
}

void dmrDebug_init()
{
    pthread_mutex_lock(&mutex);
    head        = 0;
    tail        = 0;
    used        = 0;
    dropped     = 0;
    baseTick    = getTick();
    channels    = DMR_DBG_INFO;
    initialised = true;
    pthread_mutex_unlock(&mutex);

    dmrDebug_log(DMR_DBG_INFO, "tap: DMR debug tap ready, "
                 "send '?' for status, 1-4 to toggle channels");
}

void dmrDebug_terminate()
{
    pthread_mutex_lock(&mutex);
    initialised = false;
    head        = 0;
    tail        = 0;
    used        = 0;
    pthread_mutex_unlock(&mutex);
}

void dmrDebug_enableChannels(const uint8_t chans, const bool enable)
{
    pthread_mutex_lock(&mutex);

    if(enable)
        channels |= chans;
    else
        channels &= ~chans;

    pthread_mutex_unlock(&mutex);
}

bool dmrDebug_channelEnabled(const enum dmrDebugChannel channel)
{
    if(initialised == false)
        return false;

    return (channels & ((uint8_t) channel)) != 0;
}

void dmrDebug_log(const enum dmrDebugChannel channel, const char *fmt, ...)
{
    if(dmrDebug_channelEnabled(channel) == false)
        return;

    pthread_mutex_lock(&mutex);

    int pos = sniprintf(fmtBuf, sizeof(fmtBuf), "[%08lu] ", tapTimestamp());

    va_list args;
    va_start(args, fmt);
    int num = vsniprintf(fmtBuf + pos, sizeof(fmtBuf) - pos - 3, fmt, args);
    va_end(args);

    if(num >= 0)
    {
        /* Account for the truncation performed by vsniprintf, if any */
        if(num > (int)(sizeof(fmtBuf) - pos - 3))
            num = sizeof(fmtBuf) - pos - 3;

        pos += num;
        fmtBuf[pos++] = '\r';
        fmtBuf[pos++] = '\n';

        tapPushLocked(fmtBuf, pos);
    }

    pthread_mutex_unlock(&mutex);
}

void dmrDebug_record(const enum dmrDebugChannel channel, const char *tag,
                     const void *data, const size_t len)
{
    if(dmrDebug_channelEnabled(channel) == false)
        return;

    if(len > TAP_MAX_RECORD)
    {
        dmrDebug_log(DMR_DBG_INFO, "tap: record %s too long (%u bytes)", tag,
                     (unsigned int) len);
        return;
    }

    const uint8_t *bytes = (const uint8_t *) data;
    uint8_t        sum   = 0;

    pthread_mutex_lock(&mutex);

    int pos = sniprintf(fmtBuf, sizeof(fmtBuf), "$%s %08lu ", tag,
                        tapTimestamp());

    for(size_t i = 0; i < len; i++)
    {
        fmtBuf[pos++] = hexDigits[bytes[i] >> 4];
        fmtBuf[pos++] = hexDigits[bytes[i] & 0x0F];
        sum += bytes[i];
    }

    fmtBuf[pos++] = ' ';
    fmtBuf[pos++] = hexDigits[sum >> 4];
    fmtBuf[pos++] = hexDigits[sum & 0x0F];
    fmtBuf[pos++] = '\r';
    fmtBuf[pos++] = '\n';

    tapPushLocked(fmtBuf, pos);

    pthread_mutex_unlock(&mutex);
}

void dmrDebug_mark(const char *msg)
{
    static char mark[TAP_LINE_SIZE];
    size_t      pos = 0;
    size_t      i   = 0;

    mark[pos++] = '<';
    mark[pos++] = '<';
    mark[pos++] = ' ';

    while((msg[i] != '\0') && (pos < (sizeof(mark) - 2)))
    {
        mark[pos++] = msg[i];
        i += 1;
    }

    mark[pos++] = '\r';
    mark[pos++] = '\n';

    tapWrite(mark, pos);
}

void dmrDebug_task()
{
    if(initialised == false)
        return;

    char   chunk[TAP_CHUNK_SIZE];
    size_t num  = 0;
    size_t lost = 0;

    /*
     * Copy the pending data out of the buffer before sending it: the host link
     * can block for a long time and holding the mutex across it would stall
     * every thread trying to log.
     */
    pthread_mutex_lock(&mutex);

    while((used > 0) && (num < sizeof(chunk)))
    {
        chunk[num++] = buf[tail];
        tail = (tail + 1) % TAP_BUF_SIZE;
        used--;
    }

    lost    = dropped;
    dropped = 0;

    pthread_mutex_unlock(&mutex);

    if(num > 0)
        tapWrite(chunk, num);

    /* Never let a buffer overrun pass silently, it would look like a gap */
    if(lost > 0)
        dmrDebug_log(DMR_DBG_INFO, "tap: dropped %u bytes", (unsigned int) lost);

    tapPollCommands();
}

#endif /* CONFIG_DMR */
