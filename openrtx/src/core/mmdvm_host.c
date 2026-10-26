/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/mmdvm_host.h"
#include "core/state.h"
#include "interfaces/delays.h"
#include "interfaces/platform.h"
#include "protocols/M17/m17.h"
#include "rtx/rtx.h"
#include <errno.h>
#include <stdio.h>
#include <string.h>

/*
 * Frame layout: START | length | type | payload. The length counts the whole
 * frame. Lengths above 255 are encoded as a zero length byte followed by
 * (length + 4 - 255).
 */
#define MMDVM_FRAME_START 0xE0

#define MMDVM_GET_VERSION 0x00
#define MMDVM_GET_STATUS 0x01
#define MMDVM_SET_CONFIG 0x02
#define MMDVM_SET_MODE 0x03
#define MMDVM_SET_FREQ 0x04
#define MMDVM_SEND_CWID 0x0A
#define MMDVM_M17_LINK_SETUP 0x45
#define MMDVM_M17_STREAM 0x46
#define MMDVM_M17_PACKET 0x47
#define MMDVM_M17_LOST 0x48
#define MMDVM_M17_EOT 0x49
#define MMDVM_ACK 0x70
#define MMDVM_NAK 0x7F

#define MMDVM_MODE_IDLE 0
#define MMDVM_MODE_M17 7

#define MMDVM_STATUS_M17_ENABLED 0x80
#define MMDVM_STATUS_TX 0x01
#define MMDVM_STATUS_CD 0x40
#define MMDVM_CONFIG_M17_ENABLE 0x40

/* NAK reasons, same values as the MMDVM firmware */
#define MMDVM_ERR_INVALID_COMMAND 1
#define MMDVM_ERR_INVALID_REQUEST 4
#define MMDVM_ERR_NO_SPACE 5

#define MMDVM_PROTOCOL_VERSION 1
#define MMDVM_STATUS_LENGTH 14
#define MMDVM_M17_FRAME_LENGTH (3 + 1 + MODEM_FRAME_MAX_LEN)

#ifndef GIT_VERSION
#define GIT_VERSION "unknown"
#endif

#define HOST_TIMEOUT_MS 2000
#define RX_BUF_SIZE 300
#define MAX_RX_FRAMES_PER_TICK 4

enum parserState {
    PS_START,
    PS_LENGTH,
    PS_LENGTH_EXT,
    PS_TYPE,
    PS_DATA,
};

static const struct chardev *dev = NULL;
static enum parserState parser;
static uint8_t rxBuf[RX_BUF_SIZE];
static size_t rxLen;
static size_t frameLen;
static long long lastHostRx;
static bool hostSeen;
static bool m17Enabled;
static uint8_t txDelay;

static const uint8_t m17TypeToMmdvm[] = {
    [M17_MODEM_LSF] = MMDVM_M17_LINK_SETUP,
    [M17_MODEM_STREAM] = MMDVM_M17_STREAM,
    [M17_MODEM_PACKET] = MMDVM_M17_PACKET,
    [M17_MODEM_EOT] = MMDVM_M17_EOT,
    [M17_MODEM_LOST] = MMDVM_M17_LOST,
};

static bool hostAlive(void)
{
    if (!hostSeen)
        return false;

    return (getTick() - lastHostRx) < HOST_TIMEOUT_MS;
}

static void sendToHost(const uint8_t *buf, size_t len)
{
    // Never push data towards a host that is not listening: on some
    // transports the write would block until it times out.
    if ((dev == NULL) || !hostAlive())
        return;

    chardev_write(dev, buf, len);
}

static void sendAck(uint8_t cmd)
{
    uint8_t reply[4] = { MMDVM_FRAME_START, 4, MMDVM_ACK, cmd };

    sendToHost(reply, sizeof(reply));
}

static void sendNak(uint8_t cmd, uint8_t err)
{
    uint8_t reply[5] = { MMDVM_FRAME_START, 5, MMDVM_NAK, cmd, err };

    sendToHost(reply, sizeof(reply));
}

static void sendVersion(void)
{
    uint8_t reply[128];
    const hwInfo_t *hwInfo = platform_getHwInfo();

    reply[0] = MMDVM_FRAME_START;
    reply[2] = MMDVM_GET_VERSION;
    reply[3] = MMDVM_PROTOCOL_VERSION;

    int n = snprintf((char *)&reply[4], sizeof(reply) - 4, "OpenRTX %s %s",
                     hwInfo->name, GIT_VERSION);
    if (n < 0)
        n = 0;
    if ((size_t)n > sizeof(reply) - 4)
        n = sizeof(reply) - 4;

    reply[1] = 4 + n;
    sendToHost(reply, reply[1]);
}

static void sendStatus(void)
{
    uint8_t reply[MMDVM_STATUS_LENGTH];
    const rtxStatus_t *rtx = rtx_getStatus();
    bool m17Host = (rtx->opMode == OPMODE_M17) && (rtx->hostModem != 0);

    memset(reply, 0, sizeof(reply));
    reply[0] = MMDVM_FRAME_START;
    reply[1] = MMDVM_STATUS_LENGTH;
    reply[2] = MMDVM_GET_STATUS;

    // Enabled modes, current mode, flags
    if (m17Enabled)
        reply[3] |= MMDVM_STATUS_M17_ENABLED;

    reply[4] = m17Host ? MMDVM_MODE_M17 : MMDVM_MODE_IDLE;

    if (rtx->opStatus == TX)
        reply[5] |= MMDVM_STATUS_TX;

    if (rtx_rxSquelchOpen())
        reply[5] |= MMDVM_STATUS_CD;

    // Transmit buffer space per mode: D-Star, DMR x2, YSF, P25, NXDN,
    // POCSAG are not supported, M17 is the last one.
    reply[13] = (uint8_t)rtx_modemTxSpace();

    sendToHost(reply, sizeof(reply));
}

static void sendFrame(const struct modemFrame *frame)
{
    uint8_t reply[MMDVM_M17_FRAME_LENGTH];

    if (frame->type > M17_MODEM_LOST)
        return;

    reply[0] = MMDVM_FRAME_START;
    reply[2] = m17TypeToMmdvm[frame->type];

    if (frame->type == M17_MODEM_LOST) {
        reply[1] = 3;
        sendToHost(reply, 3);
        return;
    }

    if (frame->len != MODEM_FRAME_MAX_LEN)
        return;

    reply[1] = MMDVM_M17_FRAME_LENGTH;
    reply[3] = (frame->flags & MODEM_FRAME_FIRST) ? 0x01 : 0x00;
    memcpy(&reply[4], frame->data, MODEM_FRAME_MAX_LEN);
    sendToHost(reply, sizeof(reply));
}

static void handleSetConfig(const uint8_t *data, size_t len)
{
    // Protocol version 1 config is 26 bytes long, version 2 is 40. Only the
    // fields meaningful for an M17-only modem are read: everything about
    // levels, inversion and other modes is handled by the radio itself.
    if (len < 26) {
        sendNak(MMDVM_SET_CONFIG, MMDVM_ERR_INVALID_REQUEST);
        return;
    }

    m17Enabled = (data[4] & MMDVM_CONFIG_M17_ENABLE) != 0;
    txDelay = data[5];

    sendAck(MMDVM_SET_CONFIG);
}

static void handleSetMode(const uint8_t *data, size_t len)
{
    if (len < 4) {
        sendNak(MMDVM_SET_MODE, MMDVM_ERR_INVALID_REQUEST);
        return;
    }

    // The operating mode is chosen on the radio: the host may only ask for
    // what is already there.
    if ((data[3] != MMDVM_MODE_IDLE) && (data[3] != MMDVM_MODE_M17)) {
        sendNak(MMDVM_SET_MODE, MMDVM_ERR_INVALID_REQUEST);
        return;
    }

    sendAck(MMDVM_SET_MODE);
}

static void handleSetFreq(const uint8_t *data, size_t len)
{
    // RX frequency at [4..7], TX frequency at [8..11], both little endian.
    // Newer hosts append RF level and POCSAG frequency, ignored here.
    if (len < 12) {
        sendNak(MMDVM_SET_FREQ, MMDVM_ERR_INVALID_REQUEST);
        return;
    }

    uint32_t rx = (uint32_t)data[4] | ((uint32_t)data[5] << 8)
                | ((uint32_t)data[6] << 16) | ((uint32_t)data[7] << 24);
    uint32_t tx = (uint32_t)data[8] | ((uint32_t)data[9] << 8)
                | ((uint32_t)data[10] << 16) | ((uint32_t)data[11] << 24);

    if ((rx == 0) || (tx == 0)) {
        sendNak(MMDVM_SET_FREQ, MMDVM_ERR_INVALID_REQUEST);
        return;
    }

    pthread_mutex_lock(&state_mutex);
    state.channel.rx_frequency = rx;
    state.channel.tx_frequency = tx;
    state.rtxCfgDirty = true;
    pthread_mutex_unlock(&state_mutex);

    sendAck(MMDVM_SET_FREQ);
}

static void handleM17Frame(const uint8_t *data, size_t len, uint8_t m17Type)
{
    struct modemFrame frame;

    frame.type = m17Type;
    frame.flags = 0;
    frame.len = MODEM_FRAME_MAX_LEN;

    if (len == MMDVM_M17_FRAME_LENGTH) {
        // Byte 3 is unused on the way in, the frame starts at byte 4.
        memcpy(frame.data, &data[4], MODEM_FRAME_MAX_LEN);
    } else if ((len == 3) && (m17Type == M17_MODEM_EOT)) {
        // Bare end of transmission: build the on-air EOT frame ourselves.
        for (size_t i = 0; i < MODEM_FRAME_MAX_LEN; i += 2) {
            frame.data[i] = 0x55;
            frame.data[i + 1] = 0x5D;
        }
    } else {
        sendNak(data[2], MMDVM_ERR_INVALID_REQUEST);
        return;
    }

    int ret = rtx_modemPushTxFrame(&frame);
    if (ret == -EAGAIN)
        sendNak(data[2], MMDVM_ERR_NO_SPACE);
    else if (ret != 0)
        sendNak(data[2], MMDVM_ERR_INVALID_REQUEST);
}

static void handleFrame(const uint8_t *data, size_t len)
{
    switch (data[2]) {
        case MMDVM_GET_VERSION:
            sendVersion();
            break;

        case MMDVM_GET_STATUS:
            sendStatus();
            break;

        case MMDVM_SET_CONFIG:
            handleSetConfig(data, len);
            break;

        case MMDVM_SET_MODE:
            handleSetMode(data, len);
            break;

        case MMDVM_SET_FREQ:
            handleSetFreq(data, len);
            break;

        case MMDVM_SEND_CWID:
            // Accepted and ignored: no CW identification in this mode.
            sendAck(MMDVM_SEND_CWID);
            break;

        case MMDVM_M17_LINK_SETUP:
            handleM17Frame(data, len, M17_MODEM_LSF);
            break;

        case MMDVM_M17_STREAM:
            handleM17Frame(data, len, M17_MODEM_STREAM);
            break;

        case MMDVM_M17_PACKET:
            handleM17Frame(data, len, M17_MODEM_PACKET);
            break;

        case MMDVM_M17_EOT:
            handleM17Frame(data, len, M17_MODEM_EOT);
            break;

        default:
            sendNak(data[2], MMDVM_ERR_INVALID_COMMAND);
            break;
    }
}

static void parseByte(uint8_t byte)
{
    switch (parser) {
        case PS_START:
            if (byte == MMDVM_FRAME_START) {
                rxBuf[0] = byte;
                rxLen = 1;
                parser = PS_LENGTH;
            }
            break;

        case PS_LENGTH:
            rxBuf[rxLen++] = byte;
            if (byte == 0) {
                parser = PS_LENGTH_EXT;
            } else {
                frameLen = byte;
                parser = PS_TYPE;
            }
            break;

        case PS_LENGTH_EXT:
            rxBuf[rxLen++] = byte;
            frameLen = (size_t)byte + 255;
            parser = PS_TYPE;
            break;

        case PS_TYPE:
            rxBuf[rxLen++] = byte;
            if ((frameLen < rxLen) || (frameLen > RX_BUF_SIZE)) {
                parser = PS_START;
            } else if (frameLen == rxLen) {
                handleFrame(rxBuf, rxLen);
                parser = PS_START;
            } else {
                parser = PS_DATA;
            }
            break;

        case PS_DATA:
            rxBuf[rxLen++] = byte;
            if (rxLen >= frameLen) {
                handleFrame(rxBuf, rxLen);
                parser = PS_START;
            }
            break;

        default:
            parser = PS_START;
            break;
    }
}

void mmdvmHost_init(const struct chardev *device)
{
    dev = device;
    parser = PS_START;
    rxLen = 0;
    frameLen = 0;
    hostSeen = false;
    lastHostRx = 0;
    m17Enabled = true;
    txDelay = 0;

    chardev_init(dev);
}

void mmdvmHost_terminate(void)
{
    if (dev != NULL)
        chardev_terminate(dev);

    dev = NULL;
}

void mmdvmHost_task(void)
{
    uint8_t buf[64];
    ssize_t n;

    if (dev == NULL)
        return;

    while ((n = chardev_read(dev, buf, sizeof(buf))) > 0) {
        lastHostRx = getTick();
        hostSeen = true;

        for (ssize_t i = 0; i < n; i++)
            parseByte(buf[i]);
    }

    struct modemFrame frame;
    for (int i = 0; i < MAX_RX_FRAMES_PER_TICK; i++) {
        if (rtx_modemPopRxFrame(&frame) != 0)
            break;

        sendFrame(&frame);
    }
}

bool mmdvmHost_connected(void)
{
    return hostAlive();
}
