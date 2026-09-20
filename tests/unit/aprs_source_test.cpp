/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/**
 * Integration tests for the APRS inbox source.
 *
 * The receive side is checked by calling processRx() with a crafted
 * descriptor, the way the operating mode hands a decoded frame over: an
 * addressed message for this station is stored, everything else is decoded
 * but not. The transmit side is driven through the real inbox — a message is
 * sent, and the frame that reaches the packet layer is captured by the rtx
 * stub and parsed back to prove formatTx built what was asked for.
 */

#include <catch2/catch_test_macros.hpp>

extern "C" {
#include "core/state.h"
#include "rtx/rtx.h"
}
#include "rtx_packet_stub.h"
#include "core/aprs_msg.h"
#include "core/messages.h"
#include "protocols/APRS/packet.h"

#include <cstring>

/* Provided by the test target; state.settings.callsign is what the source
 * matches received messages against and stamps into sent ones. */
extern state_t state;

namespace
{

/** Build a descriptor holding a raw frame, as the operating mode would. */
struct pktDesc frameDesc(uint8_t *buf, size_t len)
{
    struct pktDesc d = {};
    d.buffer = buf;
    d.size = len;
    d.res = (ssize_t)len;
    d.status = PKT_STATUS_DONE;
    return d;
}

void setCallsign(const char *call)
{
    strncpy(state.settings.callsign, call, sizeof(state.settings.callsign) - 1);
    state.settings.callsign[sizeof(state.settings.callsign) - 1] = '\0';
}

/** Send a message and return the frame the source built for it. */
size_t sendAndCapture(const char *sender, const char *recipient,
                      const char *body, uint8_t *out, size_t cap)
{
    rtxStub_reset();

    struct message msg = {};
    msg.body = body;
    msg.bodyLen = (uint16_t)strlen(body);
    msg.mode = OPMODE_APRS;
    strncpy(msg.sender, sender, sizeof(msg.sender) - 1);
    strncpy(msg.recipient, recipient, sizeof(msg.recipient) - 1);

    if (messages_send(&msg) != 0)
        return 0;

    struct pktDesc *tx = rtxStub_txDesc();
    if ((tx == nullptr) || (tx->size == 0) || (tx->size > cap))
        return 0;

    memcpy(out, tx->buffer, tx->size);
    return tx->size;
}

} // namespace

TEST_CASE("aprs source: an addressed message for us is stored", "[aprs][src]")
{
    messages_init();
    messages_registerSource(&aprs_msg_ops);
    messages_task(OPMODE_APRS); /* bring the inbox context up */
    setCallsign("N0CALL");

    uint8_t frame[APRS_PACLEN];
    const char info[] = ":N0CALL   :hi there";
    size_t len = aprsFrameBuild(frame, sizeof(frame), "APRS", "N2BP-7",
                                "WIDE1-1", info, strlen(info));
    REQUIRE(len > 0);

    struct pktDesc d = frameDesc(frame, len);
    size_t before = messages_count();
    REQUIRE(aprs_msg_ops.processRx(&d) == 0);
    REQUIRE(messages_count() == before + 1);

    const struct message *m = messages_get(0);
    REQUIRE(m != nullptr);
    REQUIRE(strcmp(m->sender, "N2BP-7") == 0);
    REQUIRE(strcmp(m->body, "hi there") == 0);
    messages_terminate();
}

TEST_CASE("aprs source: traffic that is not for us is not stored",
          "[aprs][src]")
{
    messages_init();
    messages_registerSource(&aprs_msg_ops);
    messages_task(OPMODE_APRS); /* bring the inbox context up */
    setCallsign("N0CALL");

    uint8_t frame[APRS_PACLEN];

    SECTION("a position report")
    {
        const char info[] = "!4903.50N/07201.75W-";
        size_t len = aprsFrameBuild(frame, sizeof(frame), "APRS", "N2BP-7",
                                    nullptr, info, strlen(info));
        struct pktDesc d = frameDesc(frame, len);
        REQUIRE(aprs_msg_ops.processRx(&d) == 0);
    }

    SECTION("a message for someone else")
    {
        const char info[] = ":W1AW     :not for us";
        size_t len = aprsFrameBuild(frame, sizeof(frame), "APRS", "N2BP-7",
                                    nullptr, info, strlen(info));
        struct pktDesc d = frameDesc(frame, len);
        REQUIRE(aprs_msg_ops.processRx(&d) == 0);
    }

    REQUIRE(messages_count() == 0);
    messages_terminate();
}

TEST_CASE("aprs source: a sent message becomes a well-formed frame",
          "[aprs][src]")
{
    messages_init();
    messages_registerSource(&aprs_msg_ops);
    messages_task(OPMODE_APRS); /* bring the inbox context up */
    setCallsign("K1ABC");

    uint8_t frame[APRS_PACLEN];
    size_t len = sendAndCapture("K1ABC", "W1AW", "meet on 146.52", frame,
                                sizeof(frame));
    REQUIRE(len > 0);

    struct aprsPacket pkt;
    REQUIRE(aprsPktFromFrame(frame, len, &pkt) == true);

    char addr[APRS_ADDR_STR_LEN];
    aprsAddrToStr(&pkt.addresses[0], addr, sizeof(addr));
    REQUIRE(strcmp(addr, APRS_TOCALL) == 0);
    aprsAddrToStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(strcmp(addr, "K1ABC") == 0);

    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];
    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(
        aprsMsgUnwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == true);
    REQUIRE(strcmp(addressee, "W1AW") == 0);
    REQUIRE(strcmp(text, "meet on 146.52") == 0);
    /* No acknowledgement solicited. */
    REQUIRE(strchr(pkt.info, '{') == nullptr);
    messages_terminate();
}

TEST_CASE("aprs source: sending is refused where it should be", "[aprs][src]")
{
    messages_init();
    messages_registerSource(&aprs_msg_ops);
    messages_task(OPMODE_APRS); /* bring the inbox context up */

    uint8_t frame[APRS_PACLEN];

    SECTION("under the factory callsign")
    {
        REQUIRE(sendAndCapture("N0CALL", "W1AW", "hi", frame, sizeof(frame))
                == 0);
    }

    SECTION("text longer than APRS allows")
    {
        char big[APRS_MSG_TEXT_MAX + 2];
        memset(big, 'x', sizeof(big) - 1);
        big[sizeof(big) - 1] = '\0';
        REQUIRE(sendAndCapture("K1ABC", "W1AW", big, frame, sizeof(frame))
                == 0);
    }

    messages_terminate();
}
