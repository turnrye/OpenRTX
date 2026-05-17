/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * M17 SMS loopback integration test.
 *
 * Exercises the full RX and TX paths of m17_sms by:
 *   - Using loopback stubs (m17_sms_loopback_stubs.cpp) that expose the live
 *     pktDesc pointer via loopback_get_rx_desc().
 *   - Injecting formatted SMS payloads directly into the RX buffer and
 *     marking the descriptor PKT_STATUS_DONE, then calling m17_sms_task()
 *     to drive reception through the same code path used on real hardware.
 *   - Verifying TX entries are advanced to MSG_STATUS_SENT after a task tick.
 */

#include <catch2/catch_test_macros.hpp>
#include <cstring>

extern "C" {
#include "hwconfig.h"
}
#include "core/m17_sms.h"
#include "protocols/M17/SmsPacket.hpp"
#include "rtx/rtx.h"

#ifdef CONFIG_M17

/* Declared in m17_sms_loopback_stubs.cpp */
extern "C" struct pktDesc *loopback_get_rx_desc(void);
extern "C" struct pktDesc *loopback_get_tx_desc(void);

/**
 * Format @p text as an M17 SMS packet into the live RX descriptor buffer
 * and mark the descriptor PKT_STATUS_DONE so the next m17_sms_task() call
 * processes it as a received packet.
 */
static bool loopback_inject(const char *text, size_t len)
{
    struct pktDesc *desc = loopback_get_rx_desc();
    if (desc == nullptr || desc->buffer == nullptr)
        return false;
    size_t written = M17::sms_format_packet(
        text, len, static_cast<uint8_t *>(desc->buffer), desc->size);
    if (written == 0)
        return false;
    desc->res = static_cast<ssize_t>(written);
    desc->status = PKT_STATUS_DONE;
    return true;
}

TEST_CASE("m17_sms loopback: TX send is promoted to SENT after task tick",
          "[m17_sms][loopback]")
{
    m17_sms_init();
    m17_sms_task(); /* arms RX descriptor */

    REQUIRE(m17_sms_send("hello", 5, "N0CALL") == 0);
    REQUIRE(m17_sms_vtable.count(NULL) == 1);

    m17_sms_task(); /* tx_desc.status==DONE → entry advanced to MSG_STATUS_SENT */

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);
    REQUIRE(hdr->direction == MSG_DIR_TX);
    REQUIRE(hdr->status == MSG_STATUS_SENT);
    REQUIRE(strcmp(static_cast<const char *>(hdr->body), "hello") == 0);
}

TEST_CASE("m17_sms loopback: TX destination is stored in packet descriptor",
          "[m17_sms][loopback]")
{
    m17_sms_init();
    m17_sms_task();

    REQUIRE(m17_sms_send("hi", 2, "W1AW") == 0);

    struct pktDesc *desc = loopback_get_tx_desc();
    REQUIRE(desc != nullptr);
    REQUIRE(strcmp(desc->destination, "W1AW") == 0);
}

TEST_CASE("m17_sms loopback: injected RX packet creates inbox entry",
          "[m17_sms][loopback]")
{
    m17_sms_init();
    m17_sms_task(); /* arms RX — loopback_get_rx_desc() is now valid */

    REQUIRE(loopback_inject("world", 5) == true);
    m17_sms_task(); /* rx_desc.status==DONE → parse + ingest */

    REQUIRE(m17_sms_vtable.count(NULL) == 1);

    message_header_t *hdr = m17_sms_vtable.get(NULL, 0);
    REQUIRE(hdr != nullptr);
    REQUIRE(hdr->direction == MSG_DIR_RX);
    REQUIRE(hdr->status == MSG_STATUS_RECEIVED);
    REQUIRE(hdr->unread == true);
    REQUIRE(strcmp(hdr->sender, "W1AW") == 0);
    REQUIRE(strcmp(static_cast<const char *>(hdr->body), "world") == 0);
}

TEST_CASE("m17_sms loopback: TX then RX round-trip yields two entries",
          "[m17_sms][loopback]")
{
    m17_sms_init();
    m17_sms_task(); /* arms RX */

    REQUIRE(m17_sms_send("ping", 4, "REMOTE") == 0);
    m17_sms_task(); /* TX done → SENT; RX descriptor still armed */

    REQUIRE(loopback_inject("pong", 4) == true);
    m17_sms_task(); /* RX done → new inbox entry; RX re-armed */

    REQUIRE(m17_sms_vtable.count(NULL) == 2);

    message_header_t *tx_hdr = nullptr;
    message_header_t *rx_hdr = nullptr;
    for (size_t i = 0; i < 2; i++) {
        message_header_t *h = m17_sms_vtable.get(NULL, i);
        REQUIRE(h != nullptr);
        if (h->direction == MSG_DIR_TX)
            tx_hdr = h;
        else
            rx_hdr = h;
    }
    REQUIRE(tx_hdr != nullptr);
    REQUIRE(rx_hdr != nullptr);
    REQUIRE(tx_hdr->status == MSG_STATUS_SENT);
    REQUIRE(strcmp(static_cast<const char *>(tx_hdr->body), "ping") == 0);
    REQUIRE(rx_hdr->status == MSG_STATUS_RECEIVED);
    REQUIRE(strcmp(static_cast<const char *>(rx_hdr->body), "pong") == 0);
    REQUIRE(strcmp(rx_hdr->sender, "W1AW") == 0);
}

TEST_CASE("m17_sms loopback: RX descriptor is re-armed after receipt",
          "[m17_sms][loopback]")
{
    m17_sms_init();
    m17_sms_task(); /* arms RX */

    REQUIRE(loopback_get_rx_desc() != nullptr);
    REQUIRE(loopback_inject("first", 5) == true);
    m17_sms_task(); /* receives first message; re-arms RX */

    /* Stub updates g_rx_desc on each rtx_addPacketRx() call */
    struct pktDesc *desc = loopback_get_rx_desc();
    REQUIRE(desc != nullptr);
    REQUIRE(desc->status == PKT_STATUS_IDLE); /* descriptor is freshly armed */

    REQUIRE(loopback_inject("second", 6) == true);
    m17_sms_task(); /* receives second message */

    REQUIRE(m17_sms_vtable.count(NULL) == 2);
}

#endif /* CONFIG_M17 */
