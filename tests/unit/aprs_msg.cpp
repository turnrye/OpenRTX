/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <cstring>

#include "aprs_test_frame.h"
#include "core/aprs_msg.h"
#include "core/messages.h"
#include "core/state.h"
#include "protocols/APRS/packet.h"
#include "rtx_packet_stub.h"

/* The source matches received messages against the APRS callsign. */
state_t state;

/* Frames handed to the KISS emitter, which the test stands in for. */
static size_t kissFrames;

extern "C" void packetEngine_send(const uint8_t *frame, size_t len)
{
    (void)frame;
    (void)len;
    kissFrames++;
}

/* Time as the source sees it, in milliseconds. */
static long long now;

extern "C" long long getTick()
{
    return now;
}

/**
 * Reset the inbox and activate it for APRS, as the UI thread would, with
 * this station's callsign set.
 */
static void setup(const char *callsign)
{
    rtxStub_reset();
    messages_init();
    REQUIRE(messages_registerSource(&aprs_msg_ops) == 0);
    messages_task(OPMODE_APRS);

    // Past every repeat window, so earlier tests leave nothing behind
    now += 24LL * 60 * 60 * 1000;

    memset(state.settings.aprs_callsign, 0,
           sizeof(state.settings.aprs_callsign));
    strncpy(state.settings.aprs_callsign, callsign,
            sizeof(state.settings.aprs_callsign) - 1);
}

/**
 * Build a UI frame from src to APRS carrying info.
 */
static struct aprsTestFrame makeFrame(const char *src, const char *info)
{
    return aprsTest_makeFrame("APRS", 0, src, 0, info);
}

/**
 * Send a message from the inbox, as the compose screen does, and return the
 * frame it hands to the rtx stage; the frame is empty if none was.
 */
static struct aprsTestFrame sendMessage(const char *recipient, const char *body)
{
    struct aprsTestFrame frame = {};
    struct message msg = {};

    msg.body = body;
    msg.bodyLen = (uint16_t)strlen(body);
    msg.mode = OPMODE_APRS;
    strncpy(msg.sender, "W9XYZ", sizeof(msg.sender) - 1);
    strncpy(msg.recipient, recipient, sizeof(msg.recipient) - 1);

    if (messages_send(&msg) != 0)
        return frame;

    struct pktDesc *tx = rtxStub_txDesc();
    if (tx == nullptr)
        return frame;

    memcpy(frame.data, tx->buffer, tx->size);
    frame.len = tx->size;
    return frame;
}

/**
 * Hand a received frame to the source, as the inbox does.
 */
static int receive(struct aprsTestFrame &frame)
{
    struct pktDesc desc = {};

    desc.status = PKT_STATUS_DONE;
    desc.buffer = frame.data;
    desc.size = sizeof(frame.data);
    desc.res = (ssize_t)frame.len;

    return aprs_msg_ops.processRx(&desc);
}

TEST_CASE("aprs_msg: source serves the APRS opmode", "[aprs][messages]")
{
    CHECK(aprs_msg_ops.mode == OPMODE_APRS);
    CHECK(strcmp(aprs_msg_ops.name, "APRS") == 0);
    CHECK(aprs_msg_ops.broadcastAddr == nullptr);
}

TEST_CASE("aprs_msg: a message for this station is filed in the inbox",
          "[aprs][messages]")
{
    setup("N0CALL");

    struct aprsTestFrame frame = makeFrame("W1AW", ":N0CALL   :Hello{01");
    REQUIRE(receive(frame) == 0);
    REQUIRE(messages_count() == 1);

    const struct message *msg = messages_get(0);
    CHECK(strcmp(msg->body, "Hello") == 0);
    CHECK(strcmp(msg->sender, "W1AW") == 0);
    CHECK(strcmp(msg->recipient, "N0CALL") == 0);
    CHECK(msg->mode == OPMODE_APRS);
    CHECK(msg->direction == MSG_DIR_RX);
    CHECK(msg->unread != 0);

    messages_terminate();
}

TEST_CASE("aprs_msg: the addressee is matched as an address",
          "[aprs][messages]")
{
    setup("N0CALL-7");

    SECTION("written in lower case with a leading zero")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", ":n0call-07:Hello");
        REQUIRE(receive(frame) == 0);
        CHECK(messages_count() == 1);
    }

    SECTION("another SSID of the same callsign is another station")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", ":N0CALL-9 :Hello");
        REQUIRE(receive(frame) == 0);
        CHECK(messages_count() == 0);
    }

    SECTION("the bare callsign is SSID 0, another station")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", ":N0CALL   :Hello");
        REQUIRE(receive(frame) == 0);
        CHECK(messages_count() == 0);
    }

    messages_terminate();
}

TEST_CASE("aprs_msg: copies of a message are stored once", "[aprs][messages]")
{
    setup("N0CALL");

    SECTION("a numbered message repeated until acknowledged")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", ":N0CALL   :Hello{01");
        REQUIRE(receive(frame) == 0);
        now += 10 * 60 * 1000;
        REQUIRE(receive(frame) == 0);
        CHECK(messages_count() == 1);
    }

    SECTION("a plain message relayed by a digipeater")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", ":N0CALL   :Hello");
        REQUIRE(receive(frame) == 0);
        now += 5 * 1000;
        REQUIRE(receive(frame) == 0);
        CHECK(messages_count() == 1);
    }

    SECTION("the same text sent again later is a new message")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", ":N0CALL   :Hello");
        REQUIRE(receive(frame) == 0);
        now += 60 * 1000;
        REQUIRE(receive(frame) == 0);
        CHECK(messages_count() == 2);
    }

    SECTION("another message number is another message")
    {
        struct aprsTestFrame first = makeFrame("W1AW", ":N0CALL   :Hello{01");
        struct aprsTestFrame second = makeFrame("W1AW", ":N0CALL   :Hello{02");
        REQUIRE(receive(first) == 0);
        REQUIRE(receive(second) == 0);
        CHECK(messages_count() == 2);
    }

    messages_terminate();
}

TEST_CASE("aprs_msg: other traffic is decoded but not stored",
          "[aprs][messages]")
{
    setup("N0CALL");

    SECTION("a position report")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", "!4903.50N/07201.75W-");
        REQUIRE(receive(frame) == 0);
    }

    SECTION("a message for another station")
    {
        struct aprsTestFrame frame = makeFrame("W1AW", ":K1ABC    :not for us");
        REQUIRE(receive(frame) == 0);
    }

    CHECK(messages_count() == 0);
    messages_terminate();
}

TEST_CASE("aprs_msg: a malformed frame is refused", "[aprs][messages]")
{
    setup("N0CALL");

    struct aprsTestFrame frame = makeFrame("W1AW", ":N0CALL   :Hello");
    frame.len = 10;

    CHECK(receive(frame) == -EINVAL);
    CHECK(messages_count() == 0);
    messages_terminate();
}

TEST_CASE("aprs_msg: a sent message goes out from the APRS callsign",
          "[aprs][messages]")
{
    setup("K1ABC-7");

    struct aprsTestFrame frame = sendMessage("W1AW", "meet on 146.52");
    REQUIRE(frame.len > 0);

    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame.data, frame.len, &pkt) == 0);

    char addr[APRS_ADDR_STR_LEN];
    aprsAddr_toStr(&pkt.addresses[0], addr, sizeof(addr));
    CHECK(strcmp(addr, APRS_TOCALL) == 0);
    aprsAddr_toStr(&pkt.addresses[1], addr, sizeof(addr));
    CHECK(strcmp(addr, "K1ABC-7") == 0);

    char path[32];
    aprsPath_toStr(&pkt, path, sizeof(path));
    CHECK(strcmp(path, APRS_DEFAULT_PATH) == 0);

    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_MSG_TEXT_MAX + 1];
    REQUIRE(
        aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == 0);
    CHECK(strcmp(addressee, "W1AW") == 0);
    CHECK(strcmp(text, "meet on 146.52") == 0);

    messages_terminate();
}

TEST_CASE("aprs_msg: sending is refused where it should be", "[aprs][messages]")
{
    SECTION("under the factory callsign")
    {
        setup("N0CALL");
        CHECK(sendMessage("W1AW", "hi").len == 0);
    }

    SECTION("text longer than an APRS message carries")
    {
        char big[APRS_MSG_TEXT_MAX + 2];

        setup("K1ABC");
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        CHECK(sendMessage("W1AW", big).len == 0);
    }

    messages_terminate();
}

TEST_CASE("aprs_msg: every frame heard goes to the KISS emitter",
          "[aprs][messages]")
{
    setup("N0CALL");
    kissFrames = 0;

    struct aprsTestFrame mine = makeFrame("W1AW", ":N0CALL   :Hello");
    struct aprsTestFrame other = makeFrame("W1AW", "!4903.50N/07201.75W-");
    REQUIRE(receive(mine) == 0);
    REQUIRE(receive(other) == 0);

    CHECK(kissFrames == 2);
    CHECK(messages_count() == 1);
    messages_terminate();
}
