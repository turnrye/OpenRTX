/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>
#include <cstdint>
#include <string>
#include "protocols/DMR/VoiceSequence.h"

/*
 * Run a transmission slot by slot, asking it to stop after a number of slots,
 * and write it down: '.' idle, 'H' header, 'h' last header, A-F voice frames,
 * 'T' terminator, 'E' end.
 */
static std::string run(int headers, int stopAfter, int maxSlots = 200,
                       bool bursts = false)
{
    struct dmrTxSequence seq;
    dmrTx_start(&seq, headers);
    std::string out;

    for (int n = 0; n < maxSlots; n++) {
        struct dmrTxBurst b = bursts ? dmrTx_nextBurst(&seq, n >= stopAfter) :
                                       dmrTx_nextSlot(&seq, n >= stopAfter);
        switch (b.type) {
            case DMR_TX_IDLE:
                out += '.';
                break;
            case DMR_TX_HEADER:
                out += b.lastHeader ? 'h' : 'H';
                break;
            case DMR_TX_VOICE:
                out += (char)('A' + b.frame);
                break;
            case DMR_TX_TERMINATOR:
                out += 'T';
                break;
            case DMR_TX_END:
                out += 'E';
                return out;
        }
    }
    return out;
}

TEST_CASE("DMR transmission opens with headers on alternate slots", "[dmr][tx]")
{
    REQUIRE(run(3, 1000).substr(0, 8) == "H.H.h.A.");
    REQUIRE(run(1, 1000).substr(0, 4) == "h.A.");
}

TEST_CASE("DMR transmission sends whole superframes and terminates after F",
          "[dmr][tx]")
{
    // Asked to stop during the headers: still one full superframe
    REQUIRE(run(3, 2) == "H.H.h.A.B.C.D.E.F.T.E");

    // Asked to stop in the middle of the second superframe
    REQUIRE(run(3, 22) == "H.H.h.A.B.C.D.E.F.A.B.C.D.E.F.T.E");

    // Asked to stop right after an F: terminator at once
    REQUIRE(run(3, 17) == "H.H.h.A.B.C.D.E.F.T.E");
}

TEST_CASE("DMR transmission never sends two slots in a row", "[dmr][tx]")
{
    std::string s = run(3, 150);
    for (size_t i = 0; i + 1 < s.size(); i++) {
        bool sent = (s[i] != '.');
        bool next = (s[i + 1] != '.');
        REQUIRE_FALSE((sent && next && s[i + 1] != 'E'));
    }
    REQUIRE(s.back() == 'E');
}

TEST_CASE(
    "DMR transmission through a repeater takes a burst per slot of its own",
    "[dmr][tx]")
{
    // The same bursts as in direct mode, without the idle slots in between
    REQUIRE(run(3, 1, 200, true) == "HHhABCDEFTE");
    REQUIRE(run(3, 11, 200, true) == "HHhABCDEFABCDEFTE");
}
