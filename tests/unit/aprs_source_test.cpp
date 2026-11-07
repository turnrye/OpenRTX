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
#include "core/aprs_traffic.h"
#include "core/stations.h"
#include "protocols/M17/m17.h"
#include "core/messages.h"
#include "protocols/APRS/packet.h"

#include <cstring>
#include <fstream>
#include <set>
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

TEST_CASE("aprs source: every packet reaches the traffic log",
          "[aprs][src][traffic]")
{
    start("K1ABC");
    aprsTraffic_init();

    hear("N2BP-7", "!4903.50N/07201.75W-test");
    hear("W1AW", ":N2BP-7   :not for us");
    hear("N2BP-7", ":K1ABC    :for us{01");
    fakeTick += 9000;
    hear("N2BP-7", ":K1ABC    :for us{01");
    send("W1AW", "ours");

    REQUIRE(aprsTraffic_count() == 5);
    CHECK(aprsTraffic_get(4)->flags == 0);
    CHECK(aprsTraffic_get(3)->flags == 0);
    CHECK(aprsTraffic_get(2)->flags == APRS_TRAFFIC_FOR_ME);
    CHECK(aprsTraffic_get(1)->flags
          == (APRS_TRAFFIC_FOR_ME | APRS_TRAFFIC_DUP));
    CHECK(aprsTraffic_get(0)->flags == APRS_TRAFFIC_TX);
    CHECK(std::string(aprsTraffic_get(0)->line)
          == "K1ABC>APORT1,WIDE1-1,WIDE2-1::W1AW     :ours{01}");

    /* The inbox still holds the message once. */
    CHECK(messages_count() == 2);
    messages_terminate();
}

TEST_CASE("aprs source: messages and acks take the configured path",
          "[aprs][src]")
{
    start("K1ABC");
    aprsTraffic_init();
    strcpy(state.settings.aprs_path, "WIDE2-1");

    /* Digipeater path of the frame last handed to the radio. */
    auto txPath = []() {
        struct pktDesc *tx = rtxStub_txDesc();
        REQUIRE(tx != nullptr);
        struct aprsPacket pkt;
        REQUIRE(aprsPkt_fromFrame(static_cast<const uint8_t *>(tx->buffer),
                                  tx->size, &pkt)
                == 0);
        char path[64];
        aprsPath_toStr(&pkt, path, sizeof(path));
        return std::string(path);
    };

    send("W1AW", "ours");
    CHECK(lastTx() == "K1ABC>:W1AW     :ours{01}");
    CHECK(txPath() == "WIDE2-1");
    CHECK(std::string(aprsTraffic_get(0)->line)
          == "K1ABC>APORT1,WIDE2-1::W1AW     :ours{01}");
    complete();

    hear("N2BP-7", ":K1ABC    :hi{42");
    messages_task(OPMODE_APRS);
    CHECK(lastTx() == "K1ABC>:N2BP-7   :ack42");
    CHECK(txPath() == "WIDE2-1");
    complete();

    /* Nothing configured falls back to the conventional default. */
    state.settings.aprs_path[0] = '\0';
    send("W1AW", "more");
    CHECK(txPath() == "WIDE1-1,WIDE2-1");
    messages_terminate();
}

TEST_CASE("aprs source: an emergency raises an alarm until it is seen",
          "[aprs][src][emergency]")
{
    start("K1ABC");
    aprsTraffic_init();
    stations_init();

    char call[APRS_ADDR_STR_LEN];
    uint32_t since = 0;
    CHECK(aprs_msg_emergency(call, sizeof(call), &since) == false);

    hear("N2BP-7", ">EMERGENCY stuck on the ridge");
    REQUIRE(aprs_msg_emergency(call, sizeof(call), &since) == true);
    CHECK(std::string(call) == "N2BP-7");
    CHECK(since == (uint32_t)(fakeTick / 1000));
    CHECK(aprsTraffic_get(0)->flags == APRS_TRAFFIC_EMERGENCY);
    CHECK((stations_find("N2BP-7")->valid & STATION_EMERGENCY) != 0);

    /* Seen: quiet, and the same station's repeat stays quiet for a while */
    aprs_msg_emergencyAck();
    CHECK(aprs_msg_emergency(nullptr, 0, nullptr) == false);
    fakeTick += 60000;
    hear("N2BP-7", ">EMERGENCY stuck on the ridge");
    CHECK(aprs_msg_emergency(nullptr, 0, nullptr) == false);

    /* Another station's emergency is a new alarm */
    hear("W1AW", ":K1ABC    :EMERGENCY come now");
    REQUIRE(aprs_msg_emergency(call, sizeof(call), nullptr) == true);
    CHECK(std::string(call) == "W1AW");
    CHECK(aprsTraffic_get(0)->flags
          == (APRS_TRAFFIC_EMERGENCY | APRS_TRAFFIC_FOR_ME));
    /* and, being addressed to us, it is in the inbox too */
    CHECK(messages_count() == 1);

    /* The flag follows the station's latest packet */
    hear("N2BP-7", "!4903.50N/07201.75W-all fine now");
    CHECK((stations_find("N2BP-7")->valid & STATION_EMERGENCY) == 0);
    CHECK((stations_find("N2BP-7")->valid & STATION_HAS_POSITION) != 0);

    /* A bulletin is marked as such and not stored */
    hear("W1AW", ":BLN1     :net tonight 8pm");
    CHECK(aprsTraffic_get(0)->flags == APRS_TRAFFIC_GROUP);
    CHECK(messages_count() == 1);

    aprs_msg_reset();
    CHECK(aprs_msg_emergency(nullptr, 0, nullptr) == false);
    messages_terminate();
}

TEST_CASE("aprs source: APRS in M17 packet mode is heard and acked over M17",
          "[aprs][src][m17]")
{
    start("K1ABC");
    aprsTraffic_init();
    stations_init();
    strcpy(state.settings.callsign, "K1ABC");

    /* N2BP-7 sends us a numbered message as an M17 packet */
    static struct m17Packet rx;
    memset(&rx, 0, sizeof(rx));
    strcpy(rx.src, "N2BP");
    strcpy(rx.dst, "ALL");
    const char *line = "N2BP-7>APORT1::K1ABC    :over m17{07";
    rx.payload[0] = 0x02;
    strcpy(reinterpret_cast<char *>(&rx.payload[1]), line);
    struct pktDesc d = {};
    d.buffer = &rx;
    d.size = sizeof(rx);
    d.res = (ssize_t)(strlen(line) + 2);
    d.status = PKT_STATUS_DONE;

    REQUIRE(aprs_msg_ops.processGuest(OPMODE_M17, &d) == 0);
    REQUIRE(messages_count() == 1);
    CHECK(std::string(messages_get(0)->body) == "over m17");
    CHECK(messages_get(0)->mode == OPMODE_APRS);
    CHECK(messages_get(0)->carrier == OPMODE_M17);
    CHECK(aprsTraffic_get(0)->flags
          == (APRS_TRAFFIC_M17 | APRS_TRAFFIC_FOR_ME));
    REQUIRE(stations_find("N2BP-7") != nullptr);
    CHECK(stations_find("N2BP-7")->carrier == STATION_CARRIER_M17);

    /* The ack is not sent on the AFSK channel... */
    aprs_msg_ops.task();
    CHECK(rtxStub_txSubmissions() == 0);

    /* ...but back over M17, from our M17 callsign, with no path */
    aprs_msg_ops.guestTask(OPMODE_M17);
    REQUIRE(rtxStub_txSubmissions() == 1);
    struct pktDesc *tx = rtxStub_txDesc();
    const auto *pkt = static_cast<const struct m17Packet *>(tx->buffer);
    CHECK(std::string(pkt->src) == "K1ABC");
    CHECK(std::string(pkt->dst) == "ALL");
    REQUIRE(pkt->payload[0] == 0x02);
    const char *ack = reinterpret_cast<const char *>(&pkt->payload[1]);
    CHECK(std::string(ack) == "K1ABC>APORT1::N2BP-7   :ack07");
    CHECK(tx->size == 20 + 2 + strlen(ack));
    complete();

    /* A repeat within the window is not stored or acked again */
    d.status = PKT_STATUS_DONE;
    REQUIRE(aprs_msg_ops.processGuest(OPMODE_M17, &d) == 0);
    CHECK(messages_count() == 1);

    /* Not ours: an SMS, raw AX.25 (0x01 is for other packet radio), or a
     * packet heard in another mode */
    static struct m17Packet sms;
    memset(&sms, 0, sizeof(sms));
    sms.payload[0] = 0x05;
    sms.payload[1] = 'h';
    struct pktDesc s = {};
    s.buffer = &sms;
    s.size = sizeof(sms);
    s.res = 3;
    s.status = PKT_STATUS_DONE;
    CHECK(aprs_msg_ops.processGuest(OPMODE_M17, &s) == -EPROTO);
    sms.payload[0] = 0x01;
    CHECK(aprs_msg_ops.processGuest(OPMODE_M17, &s) == -EPROTO);
    CHECK(aprs_msg_ops.processGuest(OPMODE_FM, &d) == -EPROTO);

    messages_terminate();
}

TEST_CASE("aprs source: heard stations land in the station table",
          "[aprs][src][stations]")
{
    start("K1ABC");
    stations_init();

    hear("N2BP-7", "!4903.50N/07201.75W>088/036/A=001234 mobile");
    hear("W1AW", ">just a status");
    hear("VE3XYZ", ";LEADER   *092345z4903.50N/07201.75W>");
    fakeTick += 5000;
    hear("N2BP-7", ":K1ABC    :hi{01");

    REQUIRE(stations_count() == 4);
    CHECK(std::string(stations_get(0)->call) == "N2BP-7");
    CHECK(std::string(stations_get(1)->call) == "LEADER");
    CHECK(std::string(stations_get(2)->call) == "VE3XYZ");
    CHECK(std::string(stations_get(3)->call) == "W1AW");

    /* A message keeps the position its sender beaconed earlier */
    const station *n2bp = stations_get(0);
    CHECK(n2bp->lastHeard == 6);
    CHECK(n2bp->latitude == 49058333);
    CHECK(n2bp->speed == 67);
    CHECK(n2bp->altitude == 376);
    CHECK(n2bp->symCode == '>');
    CHECK(n2bp->valid
          == (STATION_HAS_POSITION | STATION_HAS_SYMBOL | STATION_HAS_VELOCITY
              | STATION_HAS_ALTITUDE));
    CHECK(n2bp->kind == STATION_KIND_STATION);

    CHECK(stations_get(1)->kind == STATION_KIND_OBJECT);
    CHECK(stations_get(1)->latitude == 49058333);
    CHECK(stations_get(2)->valid == 0);
    CHECK(stations_get(3)->valid == 0);

    hear("VE3XYZ", ";LEADER   _092345z4903.50N/07201.75W>");
    CHECK(stations_find("LEADER") == nullptr);
    CHECK(stations_count() == 3);
    messages_terminate();
}

TEST_CASE("aprs source: five minutes of recorded traffic",
          "[aprs][src][replay]")
{
    start("K1ABC");
    aprsTraffic_init();
    stations_init();

    /* A message for us arrives first and must outlive everything after it */
    hear("N2BP-7", ":K1ABC    :keep me{01");

    std::ifstream in(APRS_TEST_DATA "/aprs_is_sea150.txt");
    REQUIRE(in.good());

    size_t lines = 0, heard = 0, positions = 0, weather = 0, phg = 0;
    size_t freqs = 0;
    std::set<std::string> senders;
    std::string last;
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty() || (line[0] == '#'))
            continue;
        lines++;

        /* The APRS-IS path (TCPIP*, qAR, gate names) is not an RF path */
        size_t colon = line.find(':');
        size_t comma = line.find(',');
        if ((comma != std::string::npos) && (comma < colon))
            line.erase(comma, colon - comma);

        uint8_t frame[APRS_PACLEN];
        size_t n = aprsFrameFromTnc2(line.c_str(), line.size(), frame,
                                     sizeof(frame));
        if (n == 0)
            continue; /* a source no AX.25 frame can carry */

        struct pktDesc d = frameDesc(frame, n);
        INFO(line);
        CHECK(aprs_msg_ops.processRx(&d) == 0);
        fakeTick += 1500;
        heard++;

        struct aprsPacket pkt;
        REQUIRE(aprsPkt_fromFrame(frame, n, &pkt) == 0);
        char src[APRS_ADDR_STR_LEN];
        aprsAddr_toStr(&pkt.addresses[1], src, sizeof(src));
        senders.insert(src);
        last = src;

        struct aprsPosition pos;
        struct aprsWeather wx;
        if (aprsPosParse(&pkt, &pos)) {
            positions++;
            phg += pos.hasPhg ? 1 : 0;
        }
        if (aprsWeatherParse(&pkt, &wx))
            weather++;
        struct aprsFreq freq;
        if (aprsFreqParse(&pkt, &freq))
            freqs++;
    }

    printf("replay: %zu lines, %zu heard, %zu senders, %zu positions, "
           "%zu weather, %zu PHG, %zu frequencies\n",
           lines, heard, senders.size(), positions, weather, phg, freqs);

    /* A third of APRS-IS traffic has a source no AX.25 frame can carry
     * (WINLINK, D-Star gateways' "-S"/"-B"/"-D"); the rest is all heard,
     * and most of it is a position. Measured on this capture. */
    CHECK(lines == 175);
    CHECK(heard >= 119);
    CHECK(positions >= 92);
    CHECK(weather >= 21);
    CHECK(phg >= 15);
    CHECK(freqs >= 4);

    /* The message for us is still the only thing in the inbox */
    REQUIRE(messages_count() == 1);
    CHECK(std::string(messages_get(0)->body) == "keep me");

    /* The station table is full and newest first; the log holds the tail */
    CHECK(senders.size() > 64);
    CHECK(stations_count() == 64);
    CHECK(std::string(stations_get(0)->call) == last);
    CHECK(aprsTraffic_count() == 32);
    for (size_t i = 0; i < aprsTraffic_count(); i++) {
        for (const char *c = aprsTraffic_get(i)->line; *c != '\0'; c++)
            CHECK((unsigned char)*c >= 0x20);
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

TEST_CASE("aprs source: third-party traffic is taken as sent by its source",
          "[aprs][src][thirdparty]")
{
    start("K1ABC");
    aprsTraffic_init();
    stations_init();

    /* A message to us from a station AX.25 cannot name, via an iGate */
    hear("K0RET-10", "}WINLINK>APWL2K,TCPIP,K0RET-10*::K1ABC    :mail{AB");
    REQUIRE(messages_count() == 1);
    CHECK(std::string(messages_get(0)->sender) == "WINLINK");
    CHECK(std::string(messages_get(0)->body) == "mail");

    /* The log keeps it as heard, marked */
    const struct aprsTrafficEntry *e = aprsTraffic_get(0);
    CHECK(std::string(e->sender) == "K0RET-10");
    CHECK(e->type == APRS_TYPE_THIRD_PARTY);
    CHECK(e->flags == (APRS_TRAFFIC_THIRD_PARTY | APRS_TRAFFIC_FOR_ME));

    /* Both the gateway and the source were heard */
    CHECK(stations_find("K0RET-10") != nullptr);
    CHECK(stations_find("WINLINK") != nullptr);

    /* The ack goes to the source, for the gateway to pass on */
    messages_task(OPMODE_APRS);
    REQUIRE(rtxStub_txSubmissions() == 1);
    CHECK(lastTx() == "K1ABC>:WINLINK  :ackAB");
    complete();

    /* And a reply can be sent to it */
    uint32_t seq = send("WINLINK", "thanks");
    CHECK(lastTx() == "K1ABC>:WINLINK  :thanks{01}");
    complete();
    hear("K0RET-10", "}WINLINK>APWL2K,TCPIP,K0RET-10*::K1ABC    :ack01");
    CHECK(statusOf(seq) == MSG_STATUS_DELIVERED);

    /* A position relayed for a D-Star gateway */
    hear("K0RET-10",
         "}KD7ZYN-S>APDG01,TCPIP,qAR,K0RET-10*:!4724.83N/12219.11W&");
    const struct station *s = stations_find("KD7ZYN-S");
    REQUIRE(s != nullptr);
    CHECK((s->valid & STATION_HAS_POSITION) != 0);

    /* Malformed third-party traffic is only logged */
    size_t before = messages_count();
    hear("K0RET-10", "}garbage");
    CHECK(messages_count() == before);
    CHECK(aprsTraffic_get(0)->type == APRS_TYPE_THIRD_PARTY);
    CHECK((aprsTraffic_get(0)->flags & APRS_TRAFFIC_THIRD_PARTY) == 0);
    messages_terminate();
}

TEST_CASE("aprs source: stations keep the frequency they announce",
          "[aprs][src][freq]")
{
    start("K1ABC");
    stations_init();

    /* A repeater object: the record is the object's */
    hear("KA7UEC-10", ";146.625TN*111542z3505.07N/08948.50WrPHG6460 T107 "
                      "-060 W4BS DARC");
    const struct station *s = stations_find("146.625TN");
    REQUIRE(s != nullptr);
    CHECK((s->valid & STATION_HAS_FREQ) != 0);
    CHECK(s->freq.freq == 146625000);
    CHECK(s->freq.tone == 107);
    CHECK(s->freq.offset == -600000);
    s = stations_find("KA7UEC-10");
    REQUIRE(s != nullptr);
    CHECK((s->valid & STATION_HAS_FREQ) == 0);

    /* A station's own, in its position */
    hear("K0RET-10", "!3512.68NM08955.62W&441.112MHz M17 CAN0 M17-KCW B");
    s = stations_find("K0RET-10");
    REQUIRE(s != nullptr);
    CHECK((s->valid & STATION_HAS_FREQ) != 0);
    CHECK(s->freq.mode == APRS_FREQ_M17);
    CHECK(s->freq.freq == 441112500);

    /* A position without one keeps it */
    hear("K0RET-10", "!3512.68NM08955.62W&M17 hotspot");
    s = stations_find("K0RET-10");
    CHECK((s->valid & STATION_HAS_FREQ) != 0);
    CHECK(s->freq.freq == 441112500);

    /* A tracker that sends it in its status */
    hear("N0CALL-9", ">146.520MHz T100 Monitoring");
    s = stations_find("N0CALL-9");
    REQUIRE(s != nullptr);
    CHECK((s->valid & STATION_HAS_FREQ) != 0);
    CHECK(s->freq.freq == 146520000);
    hear("N0CALL-9", "!3500.00N/09000.00W>");
    s = stations_find("N0CALL-9");
    CHECK((s->valid & STATION_HAS_POSITION) != 0);
    CHECK((s->valid & STATION_HAS_FREQ) != 0);

    /* A new one replaces it */
    hear("N0CALL-9", ">147.120MHz");
    CHECK(stations_find("N0CALL-9")->freq.freq == 147120000);
    messages_terminate();
}
