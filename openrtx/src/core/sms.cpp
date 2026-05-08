/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include "core/sms.h"
#include "core/SMSQueue.hpp"
#include "core/state.h"
#include "protocols/M17/PacketFramer.hpp"
#include "protocols/M17/SmsPacket.hpp"
#include "rtx/rtx.h"

#include <cerrno>
#include <cstring>

/*
 * Static RX buffer sized to the maximum M17 packet payload.
 */
static uint8_t rx_buf[M17::MAX_PACKET_DATA];

/*
 * Static TX buffer: SMS protocol ID (1) + message text + NUL terminator.
 * SMS_MAX_LEN from SMSQueue covers the maximum M17 SMS body (822 bytes),
 * plus 2 bytes overhead for the protocol ID and NUL.
 */
static uint8_t tx_buf[SMSQueue::SMS_MAX_LEN + 2];

static struct pktDesc rx_desc;
static struct pktDesc tx_desc;
static bool           tx_active = false;

/*
 * Inbox for received SMS messages.
 */
static SMSQueue inbox;

/*
 * Arm the RX descriptor and submit it to the rtx layer so OpMode_M17 can
 * deposit the next incoming packet into our buffer.
 */
static void arm_rx(void)
{
    rx_desc.buffer = rx_buf;
    rx_desc.size   = sizeof(rx_buf);
    rx_desc.res    = 0;
    rx_desc.status = PKT_STATUS_SUBMITTED;
    rtx_addPacketRx(&rx_desc);
}

void sms_init(void)
{
    inbox.clear();
    tx_active = false;
    arm_rx();
}

void sms_task(void)
{
    /* Poll RX descriptor */
    if (rx_desc.status == PKT_STATUS_IDLE) {
        // Retry arming: the initial arm_rx() in sms_init() may have been
        // called before the M17 opmode was active.
        arm_rx();
    } else if (rx_desc.status == PKT_STATUS_DONE) {
        char msg[SMSQueue::SMS_MAX_LEN] = {};
        bool valid = sms_parse_packet(rx_buf, (size_t)rx_desc.res, msg,
                                      sizeof(msg));

        if (valid) {
            rtxStatus_t status = rtx_getCurrentStatus();
            bool        accept = true;

            if (state.settings.m17_sms_match_call) {
                /* Accept only if the LSF destination matches our callsign
                 * or is a broadcast ("ALL"). */
                bool callsign_match =
                    (strncmp(status.M17_dst, state.settings.callsign, 9) == 0);
                bool broadcast =
                    (strncmp(status.M17_dst, "ALL", 3) == 0
                     && status.M17_dst[3] == '\0');
                accept = callsign_match || broadcast;
            }

            if (accept)
                inbox.push(status.M17_src, msg);
        }

        arm_rx();
    } else if (rx_desc.status == PKT_STATUS_ERROR) {
        arm_rx();
    }

    /* Poll TX descriptor */
    if (tx_active) {
        if (tx_desc.status == PKT_STATUS_DONE
            || tx_desc.status == PKT_STATUS_ERROR)
            tx_active = false;
    }
}

uint8_t sms_count(void)
{
    return inbox.count();
}

bool sms_get(uint8_t index, char *sender, size_t sender_len, char *message,
             size_t message_len)
{
    return inbox.get(index, sender, sender_len, message, message_len);
}

void sms_erase(uint8_t index)
{
    inbox.erase(index);
}

int sms_send(const char *message, size_t msgLen)
{
    if (msgLen == 0)
        return -EINVAL;

    if (tx_active)
        return -EBUSY;

    size_t len =
        sms_format_packet(message, msgLen, tx_buf, sizeof(tx_buf));
    if (len == 0)
        return -EINVAL;

    tx_desc.buffer = tx_buf;
    tx_desc.size   = len;
    tx_desc.res    = 0;
    tx_desc.status = PKT_STATUS_SUBMITTED;

    int ret = rtx_addPacketTx(&tx_desc);
    if (ret != 0)
        return ret;

    tx_active = true;
    return 0;
}

bool sms_tx_pending(void)
{
    return tx_active;
}
