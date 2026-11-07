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
#include "protocols/M17/m17.h"
#include "core/messages.h"
#include "protocols/APRS/packet.h"

#include <cstring>
#include <string>

/* Provided by the test target; state.settings.aprs_callsign is what the
 * source matches received messages against. */
extern state_t state;
extern long long fakeTick;

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
    strncpy(state.settings.aprs_callsign, call,
            sizeof(state.settings.aprs_callsign) - 1);
    state.settings.aprs_callsign[sizeof(state.settings.aprs_callsign) - 1] =
        '\0';
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

/** Bring the inbox up in APRS mode with a clean slate, as @p call. */
void start(const char *call)
{
    messages_init();
    rtxStub_reset();
    aprs_msg_reset();
    fakeTick = 1000;
    messages_registerSource(&aprs_msg_ops);
    messages_task(OPMODE_APRS);
    setCallsign(call);
    state.settings.aprs_path[0] = '\0';
}

/** Hand the source a frame from @p src carrying @p info. */
void hear(const char *src, const char *info)
{
    uint8_t frame[APRS_PACLEN];
    ssize_t len = aprsFrame_build(frame, sizeof(frame), "APRS", src, "WIDE1-1",
                                  info, strlen(info));
    REQUIRE(len > 0);
    struct pktDesc d = frameDesc(frame, len);
    REQUIRE(aprs_msg_ops.processRx(&d) == 0);
}

/** Source address and info field of the last frame transmitted. */
std::string lastTx()
{
    struct pktDesc *tx = rtxStub_txDesc();
    REQUIRE(tx != nullptr);
    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(static_cast<const uint8_t *>(tx->buffer),
                              tx->size, &pkt)
            == 0);
    char src[APRS_ADDR_STR_LEN];
    aprsAddr_toStr(&pkt.addresses[1], src, sizeof(src));
    return std::string(src) + ">" + std::string(pkt.info, pkt.infoLen);
}

/** Let the radio finish the transmission in flight and run the inbox. */
void complete()
{
    struct pktDesc *tx = rtxStub_txDesc();
    if ((tx != nullptr) && (tx->status == PKT_STATUS_SUBMITTED))
        tx->status = PKT_STATUS_DONE;
    messages_task(OPMODE_APRS);
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
    ssize_t len = aprsFrame_build(frame, sizeof(frame), "APRS", "N2BP-7",
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
        ssize_t len = aprsFrame_build(frame, sizeof(frame), "APRS", "N2BP-7",
                                      nullptr, info, strlen(info));
        struct pktDesc d = frameDesc(frame, len);
        REQUIRE(aprs_msg_ops.processRx(&d) == 0);
    }

    SECTION("a message for someone else")
    {
        const char info[] = ":W1AW     :not for us";
        ssize_t len = aprsFrame_build(frame, sizeof(frame), "APRS", "N2BP-7",
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
    start("K1ABC");

    uint8_t frame[APRS_PACLEN];
    size_t len = sendAndCapture("K1ABC", "W1AW", "meet on 146.52", frame,
                                sizeof(frame));
    REQUIRE(len > 0);

    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame, len, &pkt) == 0);

    char addr[APRS_ADDR_STR_LEN];
    aprsAddr_toStr(&pkt.addresses[0], addr, sizeof(addr));
    REQUIRE(strcmp(addr, APRS_TOCALL) == 0);
    aprsAddr_toStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(strcmp(addr, "K1ABC") == 0);

    char addressee[APRS_ADDR_STR_LEN];
    char text[APRS_PACLEN];
    REQUIRE(pkt.type == APRS_TYPE_MESSAGE);
    REQUIRE(
        aprsMsg_unwrap(&pkt, addressee, sizeof(addressee), text, sizeof(text))
        == 0);
    REQUIRE(strcmp(addressee, "W1AW") == 0);
    REQUIRE(strcmp(text, "meet on 146.52") == 0);
    /* Numbered, so the recipient acknowledges it. */
    REQUIRE(std::string(pkt.info, pkt.infoLen)
            == ":W1AW     :meet on 146.52{01}");
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

TEST_CASE("aprs source: the addressee is matched as an address", "[aprs][src]")
{
    start("K1ABC-7");

    hear("N2BP", ":k1abc-7  :lower case");
    hear("N2BP", ":K1ABC    :no ssid");
    hear("N2BP", ":K1ABC-07 :leading zero");
    REQUIRE(messages_count() == 2);
    CHECK(std::string(messages_get(1)->body) == "lower case");
    CHECK(std::string(messages_get(0)->body) == "leading zero");

    start("K1ABC");
    hear("N2BP", ":K1ABC-0  :zero ssid");
    hear("N2BP", ":K1ABC-7  :other ssid");
    REQUIRE(messages_count() == 1);
    CHECK(std::string(messages_get(0)->body) == "zero ssid");
    messages_terminate();
}

TEST_CASE("aprs source: a numbered message is stored once and acked per copy",
          "[aprs][src][ack]")
{
    start("K1ABC");

    hear("N2BP-7", ":K1ABC    :hello{042");
    REQUIRE(messages_count() == 1);
    CHECK(std::string(messages_get(0)->body) == "hello");

    messages_task(OPMODE_APRS);
    REQUIRE(rtxStub_txSubmissions() == 1);
    CHECK(lastTx() == "K1ABC>:N2BP-7   :ack042");
    complete();

    /* A digipeated copy moments later: no new entry and no second ack. */
    fakeTick += 5000;
    hear("N2BP-7", ":K1ABC    :hello{042");
    messages_task(OPMODE_APRS);
    CHECK(messages_count() == 1);
    CHECK(rtxStub_txSubmissions() == 1);

    /* The sender's retry, after our ack was lost: acknowledged again. */
    fakeTick += 40000;
    hear("N2BP-7", ":K1ABC    :hello{042");
    messages_task(OPMODE_APRS);
    CHECK(messages_count() == 1);
    CHECK(rtxStub_txSubmissions() == 2);
    messages_terminate();
}

TEST_CASE("aprs source: an unnumbered message is not acked", "[aprs][src][ack]")
{
    start("K1ABC");

    hear("N2BP-7", ":K1ABC    :no reply wanted");
    messages_task(OPMODE_APRS);
    CHECK(rtxStub_txSubmissions() == 0);

    /* A digipeated copy is dropped, the same words later are a new message. */
    fakeTick += 5000;
    hear("N2BP-7", ":K1ABC    :no reply wanted");
    CHECK(messages_count() == 1);
    fakeTick += 60000;
    hear("N2BP-7", ":K1ABC    :no reply wanted");
    CHECK(messages_count() == 2);
    messages_terminate();
}

TEST_CASE("aprs source: a reply-ack number is echoed exactly",
          "[aprs][src][ack]")
{
    start("K1ABC");

    hear("N2BP-7", ":K1ABC    :hi{AB}CD");
    messages_task(OPMODE_APRS);
    CHECK(lastTx() == "K1ABC>:N2BP-7   :ackAB}CD");
    messages_terminate();
}

TEST_CASE("aprs source: no ack under the factory callsign", "[aprs][src][ack]")
{
    start("N0CALL");

    hear("N2BP-7", ":N0CALL   :hello{1");
    messages_task(OPMODE_APRS);
    CHECK(messages_count() == 1);
    CHECK(rtxStub_txSubmissions() == 0);
    messages_terminate();
}

TEST_CASE("aprs source: an ack waits for the transmitter, then goes stale",
          "[aprs][src][ack]")
{
    start("K1ABC");

    rtxStub_setTxResult(-EBUSY);
    hear("N2BP-7", ":K1ABC    :first{1");
    messages_task(OPMODE_APRS);
    CHECK(rtxStub_txSubmissions() == 0);

    rtxStub_setTxResult(0);
    fakeTick += 2000;
    messages_task(OPMODE_APRS);
    CHECK(lastTx() == "K1ABC>:N2BP-7   :ack1");
    complete();

    rtxStub_setTxResult(-EBUSY);
    hear("N2BP-7", ":K1ABC    :second{2");
    messages_task(OPMODE_APRS);
    rtxStub_setTxResult(0);
    fakeTick += 61000;
    messages_task(OPMODE_APRS);
    CHECK(rtxStub_txSubmissions() == 1);
    messages_terminate();
}

namespace
{

/** Send @p body to @p to as K1ABC and return its sequence number. */
uint32_t send(const char *to, const char *body)
{
    struct message msg = {};
    msg.body = body;
    msg.bodyLen = (uint16_t)strlen(body);
    msg.mode = OPMODE_APRS;
    strncpy(msg.sender, "K1ABC", sizeof(msg.sender) - 1);
    strncpy(msg.recipient, to, sizeof(msg.recipient) - 1);
    REQUIRE(messages_send(&msg) == 0);
    return messages_get(0)->sequence;
}

uint8_t statusOf(uint32_t seq)
{
    const struct message *m = messages_get(messages_findBySequence(seq));
    REQUIRE(m != nullptr);
    return m->status;
}

} // namespace

TEST_CASE("aprs source: our message is delivered by its ack",
          "[aprs][src][ack]")
{
    start("K1ABC");

    uint32_t seq = send("W1AW", "ping");
    CHECK(lastTx() == "K1ABC>:W1AW     :ping{01}");
    complete();
    CHECK(statusOf(seq) == MSG_STATUS_SENT);

    hear("W1AW", ":K1ABC    :ack01");
    CHECK(statusOf(seq) == MSG_STATUS_DELIVERED);
    CHECK(messages_count() == 1);

    fakeTick += 31000;
    messages_task(OPMODE_APRS);
    CHECK(rtxStub_txSubmissions() == 1);
    messages_terminate();
}

TEST_CASE("aprs source: our message is rejected by a rej", "[aprs][src][ack]")
{
    start("K1ABC");

    uint32_t seq = send("W1AW", "ping");
    complete();
    hear("W1AW", ":K1ABC    :rej01");
    CHECK(statusOf(seq) == MSG_STATUS_REJECTED);
    messages_terminate();
}

TEST_CASE("aprs source: only the recipient's ack of our number counts",
          "[aprs][src][ack]")
{
    start("K1ABC");

    uint32_t seq = send("W1AW", "ping");
    complete();
    hear("N2BP", ":K1ABC    :ack01");
    hear("W1AW", ":K1ABC    :ack02");
    CHECK(statusOf(seq) == MSG_STATUS_SENT);

    /* Acknowledgements never become messages. */
    CHECK(messages_count() == 1);
    messages_terminate();
}

TEST_CASE("aprs source: an unanswered message is retried, then given up",
          "[aprs][src][ack]")
{
    start("K1ABC");

    uint32_t seq = send("W1AW", "ping");
    complete();

    const long long gaps[] = { 35000, 60000, 120000 };
    size_t sent = 1;
    for (long long gap : gaps) {
        fakeTick += gap - 1000;
        messages_task(OPMODE_APRS);
        CHECK(rtxStub_txSubmissions() == sent);

        fakeTick += 1000;
        messages_task(OPMODE_APRS);
        CHECK(rtxStub_txSubmissions() == ++sent);
        CHECK(lastTx() == "K1ABC>:W1AW     :ping{01}");
        complete();
        CHECK(statusOf(seq) == MSG_STATUS_SENT);
    }

    fakeTick += 239000;
    messages_task(OPMODE_APRS);
    CHECK(statusOf(seq) == MSG_STATUS_SENT);
    fakeTick += 1000;
    messages_task(OPMODE_APRS);
    CHECK(statusOf(seq) == MSG_STATUS_UNACKED);
    CHECK(rtxStub_txSubmissions() == 4);
    messages_terminate();
}

TEST_CASE("aprs source: an ack in the reply-ack form delivers too",
          "[aprs][src][ack]")
{
    start("K1ABC");

    /* aprs.fi answers "{MM}" with "ackMM}" */
    uint32_t seq = send("W1AW", "ping");
    CHECK(lastTx() == "K1ABC>:W1AW     :ping{01}");
    complete();
    hear("W1AW", ":K1ABC    :ack01}");
    CHECK(statusOf(seq) == MSG_STATUS_DELIVERED);
    messages_terminate();
}

TEST_CASE("aprs source: an ack during a retry stops the retries",
          "[aprs][src][ack]")
{
    start("K1ABC");

    uint32_t seq = send("W1AW", "ping");
    complete();
    fakeTick += 35000;
    messages_task(OPMODE_APRS);
    REQUIRE(rtxStub_txSubmissions() == 2);

    hear("W1AW", ":K1ABC    :ack01");
    complete();
    CHECK(statusOf(seq) == MSG_STATUS_DELIVERED);

    fakeTick += 60000;
    messages_task(OPMODE_APRS);
    CHECK(rtxStub_txSubmissions() == 2);
    messages_terminate();
}

TEST_CASE("aprs source: with every slot waiting, the oldest is given up",
          "[aprs][src][ack]")
{
    start("K1ABC");

    uint32_t first = send("W1AW", "one");
    complete();
    for (const char *body : { "two", "three", "four" }) {
        send("W1AW", body);
        complete();
    }
    CHECK(statusOf(first) == MSG_STATUS_SENT);

    uint32_t fifth = send("W1AW", "five");
    CHECK(lastTx() == "K1ABC>:W1AW     :five{05}");
    CHECK(statusOf(first) == MSG_STATUS_UNACKED);
    complete();
    CHECK(statusOf(fifth) == MSG_STATUS_SENT);
    messages_terminate();
}

TEST_CASE("aprs source: a reply-ack in their message delivers ours",
          "[aprs][src][ack]")
{
    start("K1ABC");

    uint32_t seq = send("W1AW", "ping");
    complete();

    hear("W1AW", ":K1ABC    :pong{AB}01");
    CHECK(statusOf(seq) == MSG_STATUS_DELIVERED);
    CHECK(std::string(messages_get(0)->body) == "pong");

    /* Their message is still acknowledged on its own. */
    messages_task(OPMODE_APRS);
    CHECK(lastTx() == "K1ABC>:W1AW     :ackAB}01");
    messages_terminate();
}

TEST_CASE("aprs source: our next message carries the reply-ack owed",
          "[aprs][src][ack]")
{
    start("K1ABC");

    SECTION("to a station that sent the reply-ack form")
    {
        hear("W1AW", ":K1ABC    :hi{AB}");
        messages_task(OPMODE_APRS);
        complete();
        send("W1AW", "yo");
        CHECK(lastTx() == "K1ABC>:W1AW     :yo{01}AB");
    }

    SECTION("but not to one that sent the old form")
    {
        hear("W1AW", ":K1ABC    :hi{042");
        messages_task(OPMODE_APRS);
        complete();
        send("W1AW", "yo");
        CHECK(lastTx() == "K1ABC>:W1AW     :yo{01}");
    }

    SECTION("nor to anyone else")
    {
        hear("W1AW", ":K1ABC    :hi{AB}");
        messages_task(OPMODE_APRS);
        complete();
        send("N2BP", "yo");
        CHECK(lastTx() == "K1ABC>:N2BP     :yo{01}");
    }

    messages_terminate();
}

namespace
{

int noRx(const struct pktDesc *pkt)
{
    (void)pkt;
    return -EPROTO;
}

int noTx(const struct message *msg, struct pktDesc *pkt)
{
    (void)msg;
    (void)pkt;
    return -EIO;
}

/* Stands in for M17 SMS, so the inbox stays up in M17 mode. */
const struct messageOps m17Ops = { noRx, noTx, "M17", OPMODE_M17 };

/* The TNC2 line of the APRS packet last handed to the radio over M17. */
std::string lastM17Line()
{
    struct pktDesc *tx = rtxStub_txDesc();
    REQUIRE(tx != nullptr);
    const auto *pkt = static_cast<const struct m17Packet *>(tx->buffer);
    REQUIRE(pkt->payload[0] == 0x02);
    return std::string(reinterpret_cast<const char *>(&pkt->payload[1]));
}

} // namespace

TEST_CASE("aprs source: a message sent over M17 is numbered, retried and "
          "acked there",
          "[aprs][src][m17]")
{
    start("K1ABC-7");
    messages_registerSource(&m17Ops);
    strcpy(state.settings.callsign, "K1ABC");

    struct message msg = {};
    msg.body = "ping";
    msg.bodyLen = 4;
    msg.mode = OPMODE_APRS;
    msg.carrier = OPMODE_M17;
    strcpy(msg.sender, "K1ABC-7");
    strcpy(msg.recipient, "W1AW");
    REQUIRE(messages_send(&msg) == 0);
    uint32_t seq = messages_get(0)->sequence;

    REQUIRE(rtxStub_txSubmissions() == 1);
    CHECK(lastM17Line() == "K1ABC-7>APORT1::W1AW     :ping{01}");
    CHECK(std::string(static_cast<const char *>(rtxStub_txDesc()->buffer))
          == "K1ABC");
    rtxStub_txDesc()->status = PKT_STATUS_DONE;
    messages_task(OPMODE_M17);
    CHECK(statusOf(seq) == MSG_STATUS_SENT);

    /* Retries happen in M17 mode, never on the AFSK channel */
    fakeTick += 35000;
    messages_task(OPMODE_APRS);
    CHECK(rtxStub_txSubmissions() == 1);
    messages_task(OPMODE_M17);
    REQUIRE(rtxStub_txSubmissions() == 2);
    CHECK(lastM17Line() == "K1ABC-7>APORT1::W1AW     :ping{01}");
    rtxStub_txDesc()->status = PKT_STATUS_DONE;
    messages_task(OPMODE_M17);

    /* The ack comes back over M17 too */
    static struct m17Packet rx;
    memset(&rx, 0, sizeof(rx));
    const char *ack = "W1AW>APRS::K1ABC-7  :ack01";
    rx.payload[0] = 0x02;
    strcpy(reinterpret_cast<char *>(&rx.payload[1]), ack);
    struct pktDesc d = {};
    d.buffer = &rx;
    d.size = sizeof(rx);
    d.res = (ssize_t)(strlen(ack) + 2);
    d.status = PKT_STATUS_DONE;
    REQUIRE(aprs_msg_ops.processGuest(OPMODE_M17, &d) == 0);
    CHECK(statusOf(seq) == MSG_STATUS_DELIVERED);

    /* Nothing more to retry */
    fakeTick += 120000;
    messages_task(OPMODE_M17);
    CHECK(rtxStub_txSubmissions() == 2);
    messages_terminate();
}
