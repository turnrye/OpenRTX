/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Contract tests for the APRS beacon producer. The transmit path is the shared
 * rtx packet stub, which records submissions and returns a result the test
 * chooses, so every property can be checked without a radio — that a valid
 * report is wrapped into a position frame from the right source along the right
 * path, that the factory callsign is refused, that a busy transmit slot is
 * reported without wedging the producer, and that a beacon still in flight is
 * not allowed to overwrite the buffer under it.
 *
 * The producer keeps its descriptor in a static, and the stub exposes a
 * pointer to the last one submitted: a test marks that descriptor done to
 * model the transmit path finishing, the same transition an opmode makes, and
 * so also releases the producer's slot between tests.
 */

#include <catch2/catch_test_macros.hpp>

#include <cerrno>
#include <string>

extern "C" {
#include "core/aprs_beacon.h"
}
#include "rtx_packet_stub.h"

namespace
{

/* Start a test with a clean stub and the producer's slot free: any descriptor
 * still SUBMITTED from a previous test is completed through the stub's pointer,
 * since the producer owns it statically and would otherwise stay busy. */
void freshStart(int txResult)
{
    struct pktDesc *last = rtxStub_txDesc();
    if (last != nullptr)
        last->status = PKT_STATUS_DONE;

    rtxStub_reset();
    rtxStub_setTxResult(txResult);
}

struct aprsPosReport makeReport()
{
    struct aprsPosReport rpt;
    rpt.latitude = 33123456;
    rpt.longitude = -96987654;
    rpt.symbolTable = '/';
    rpt.symbolCode = '>';
    rpt.hasCourseSpeed = false;
    rpt.course = 0;
    rpt.speed = 0;
    rpt.hasAltitude = false;
    rpt.altitude = 0;
    rpt.comment = nullptr;
    return rpt;
}

} // namespace

TEST_CASE("APRS beacon: a valid report is wrapped and submitted",
          "[aprs][beacon]")
{
    freshStart(0);
    struct aprsPosReport rpt = makeReport();

    REQUIRE(aprsBeacon_send("W1AW-9", "WIDE1-1,WIDE2-1", &rpt) == 0);
    REQUIRE(rtxStub_txSubmissions() == 1);

    struct pktDesc *d = rtxStub_txDesc();
    REQUIRE(d != nullptr);

    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame((const uint8_t *)d->buffer, d->size, &pkt) == 0);
    REQUIRE(pkt.type == APRS_TYPE_POSITION);

    char addr[APRS_ADDR_STR_LEN];
    aprsAddr_toStr(&pkt.addresses[0], addr, sizeof(addr));
    REQUIRE(std::string(addr) == std::string(APRS_TOCALL));
    aprsAddr_toStr(&pkt.addresses[1], addr, sizeof(addr));
    REQUIRE(std::string(addr) == "W1AW-9");

    char path[32];
    aprsPath_toStr(&pkt, path, sizeof(path));
    REQUIRE(std::string(path) == "WIDE1-1,WIDE2-1");
    REQUIRE(std::string(pkt.info, pkt.infoLen) == "=3307.41N/09659.26W>");
}

TEST_CASE("APRS beacon: a null or empty path uses the default WIDE path",
          "[aprs][beacon]")
{
    freshStart(0);
    struct aprsPosReport rpt = makeReport();

    REQUIRE(aprsBeacon_send("W1AW-9", nullptr, &rpt) == 0);

    struct pktDesc *d = rtxStub_txDesc();
    struct aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame((const uint8_t *)d->buffer, d->size, &pkt) == 0);
    char path[32];
    aprsPath_toStr(&pkt, path, sizeof(path));
    REQUIRE(std::string(path) == std::string(APRS_DEFAULT_PATH));
}

TEST_CASE("APRS beacon: in M17 packet mode the frame rides an M17 packet",
          "[aprs][beacon][m17]")
{
    freshStart(0);
    struct aprsPosReport rpt = makeReport();

    REQUIRE(aprsBeacon_sendM17("K1ABC", "K1ABC-9", &rpt) == 0);
    REQUIRE(rtxStub_txSubmissions() == 1);

    struct pktDesc *d = rtxStub_txDesc();
    const char *raw = static_cast<const char *>(d->buffer);
    CHECK(std::string(raw) == "K1ABC");
    CHECK(std::string(raw + 10) == "ALL");
    const uint8_t *payload = reinterpret_cast<const uint8_t *>(raw + 20);
    REQUIRE(payload[0] == 0x02);

    /* A TNC2 line and its NUL; no digipeaters on M17 */
    std::string line(reinterpret_cast<const char *>(payload + 1));
    CHECK(line.rfind("K1ABC-9>APORT1:=", 0) == 0);
    CHECK(d->size == 20 + 2 + line.size());

    /* One beacon at a time, whichever carrier holds the slot */
    CHECK(aprsBeacon_send("K1ABC-9", nullptr, &rpt) == -EBUSY);
    CHECK(aprsBeacon_sendM17("K1ABC", "K1ABC-9", &rpt) == -EBUSY);

    /* Both identities must be set */
    freshStart(0);
    CHECK(aprsBeacon_sendM17("N0CALL", "K1ABC-9", &rpt) == -EINVAL);
    CHECK(aprsBeacon_sendM17("K1ABC", "N0CALL", &rpt) == -EINVAL);
    CHECK(aprsBeacon_sendM17("", "K1ABC-9", &rpt) == -EINVAL);
}

TEST_CASE("APRS beacon: the factory callsign is refused", "[aprs][beacon]")
{
    freshStart(0);
    struct aprsPosReport rpt = makeReport();

    REQUIRE(aprsBeacon_send("N0CALL", nullptr, &rpt) == -EINVAL);
    REQUIRE(aprsBeacon_send("N0CALL-7", nullptr, &rpt) == -EINVAL);
    REQUIRE(aprsBeacon_send("", nullptr, &rpt) == -EINVAL);
    REQUIRE(aprsBeacon_send(nullptr, nullptr, &rpt) == -EINVAL);

    REQUIRE(rtxStub_txSubmissions() == 0);
}

TEST_CASE("APRS beacon: a null report is refused", "[aprs][beacon]")
{
    freshStart(0);
    REQUIRE(aprsBeacon_send("W1AW-9", nullptr, nullptr) == -EINVAL);
    REQUIRE(rtxStub_txSubmissions() == 0);
}

TEST_CASE("APRS beacon: a busy transmit slot is reported and recoverable",
          "[aprs][beacon]")
{
    struct aprsPosReport rpt = makeReport();

    freshStart(-EBUSY);
    REQUIRE(aprsBeacon_send("W1AW-9", nullptr, &rpt) == -EBUSY);
    REQUIRE(rtxStub_txSubmissions() == 0);

    /* A refused submit must not leave the producer stuck: with the slot free
     * again, the next attempt goes through. */
    freshStart(0);
    REQUIRE(aprsBeacon_send("W1AW-9", nullptr, &rpt) == 0);
    REQUIRE(rtxStub_txSubmissions() == 1);
}

TEST_CASE("APRS beacon: a beacon in flight is not overwritten",
          "[aprs][beacon]")
{
    struct aprsPosReport rpt = makeReport();

    freshStart(0);
    REQUIRE(aprsBeacon_send("W1AW-9", nullptr, &rpt) == 0);
    REQUIRE(rtxStub_txSubmissions() == 1);

    /* The descriptor is left SUBMITTED, as the transmit path would while
     * sending: a second beacon is refused rather than clobbering it, and never
     * reaches the transmit stage. */
    REQUIRE(aprsBeacon_send("W1AW-9", nullptr, &rpt) == -EBUSY);
    REQUIRE(rtxStub_txSubmissions() == 1);

    /* Model the transmit finishing, and the next beacon goes out. */
    rtxStub_txDesc()->status = PKT_STATUS_DONE;
    REQUIRE(aprsBeacon_send("W1AW-9", nullptr, &rpt) == 0);
    REQUIRE(rtxStub_txSubmissions() == 2);
}
