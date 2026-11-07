/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * Receive an APRS packet in M17 packet mode through the chain
 * OpMode_M17::rxState uses: demodulator, frame decoder, packet deframer,
 * then the APRS payload. The packet is the one m17-gateway transmits for an
 * APRS-IS message, with its LSF TYPE of 0x0000.
 */

#include <catch2/catch_test_macros.hpp>

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "core/fir.hpp"
#include "protocols/M17/Callsign.hpp"
#include "protocols/M17/Constants.hpp"
#include "protocols/M17/Demodulator.hpp"
#include "protocols/M17/DSP.hpp"
#include "protocols/M17/FrameDecoder.hpp"
#include "protocols/M17/FrameEncoder.hpp"
#include "protocols/M17/LinkSetupFrame.hpp"
#include "protocols/M17/PacketDeframer.hpp"
#include "protocols/M17/PacketFramer.hpp"
#include "protocols/M17/Utils.hpp"
#include "protocols/M17/AprsPacket.h"
#include "protocols/APRS/packet.h"

using namespace M17;

namespace
{

constexpr size_t SPS = 5; // 24 kHz, 4800 symbols/s

void appendFrame(std::vector<int8_t> &syms, const frame_t &frame)
{
    for (uint8_t b : frame) {
        auto s = byteToSymbols(b);
        syms.insert(syms.end(), s.begin(), s.end());
    }
}

void appendPreamble(std::vector<int8_t> &syms)
{
    for (size_t i = 0; i < FRAME_SYMBOLS; i++)
        syms.push_back((i % 2 == 0) ? +3 : -3);
}

/* What m17-gateway sends: LSF from "K0RET H" to broadcast with TYPE 0x0000,
 * the APRS line as packet type 0x02, then EOT. */
void appendGatewayPacket(std::vector<int8_t> &syms, const char *line)
{
    FrameEncoder encoder;
    frame_t frame;

    LinkSetupFrame lsf;
    lsf.clear();
    lsf.setSource(Callsign("K0RET H"));
    lsf.setDestination(Callsign("ALL"));
    streamType_t type;
    type.value = 0;
    lsf.setType(type);
    lsf.updateCrc();

    appendPreamble(syms);
    encoder.reset();
    encoder.encodeLsf(lsf, frame);
    appendFrame(syms, frame);

    static uint8_t data[M17_MAX_PACKET_DATA];
    size_t len = strlen(line);
    data[0] = M17_PACKET_APRS;
    memcpy(&data[1], line, len);
    data[1 + len] = '\0';

    PacketFramer framer;
    REQUIRE(framer.init(data, len + 2));
    PacketFrame pf;
    bool last;
    do {
        last = framer.nextFrame(pf);
        encoder.encodePacketFrame(pf, frame);
        appendFrame(syms, frame);
    } while (!last);

    encoder.encodeEotFrame(frame);
    appendFrame(syms, frame);
}

std::vector<int16_t> baseband(const std::vector<int8_t> &syms)
{
    Fir<float, rrc_taps_24k.size()> rrc(rrc_taps_24k);
    std::vector<int16_t> out;

    for (int8_t s : syms) {
        out.push_back(static_cast<int16_t>(rrc(s * 2000.0f) * SPS));
        for (size_t k = 1; k < SPS; k++)
            out.push_back(static_cast<int16_t>(rrc(0.0f) * SPS));
    }

    return out;
}

/* The APRS lines received, as rxState and the APRS source would take them. */
std::vector<std::string> receive(const std::vector<int16_t> &bb)
{
    Demodulator demod;
    FrameDecoder decoder;
    PacketDeframer deframer;
    static uint8_t buf[M17_MAX_PACKET_DATA];
    std::vector<std::string> lines;
    bool locked = false;
    bool assembling = false;

    demod.init();
    const frame_t *last = &demod.getFrame();

    for (int16_t s : bb) {
        demod.sample(s);

        bool lock = demod.isLocked();
        if (lock && !locked)
            decoder.reset();

        /* The frame buffers swap once per completed frame. */
        const frame_t *frame = &demod.getFrame();
        bool newFrame = (frame != last);
        last = frame;

        if ((locked || lock) && newFrame) {
            auto type = decoder.decodeFrame(*frame);
            LinkSetupFrame lsf = decoder.getLsf();

            if (lsf.valid() && (type == FrameType::PACKET)
                && lsf.getDestination().isSpecial()) {
                if (!assembling) {
                    deframer.init(buf, sizeof(buf));
                    assembling = true;
                }

                auto r = deframer.pushFrame(decoder.getPacketFrame());
                if (r == DeframerResult::COMPLETE) {
                    uint8_t ax25[APRS_PACLEN];
                    size_t n = m17_aprsFrame(buf, deframer.length(), ax25,
                                             sizeof(ax25));
                    char line[APRS_PACLEN + 64];
                    if ((n > 0) && aprsFrameToTnc2(ax25, n, line, sizeof(line)))
                        lines.push_back(line);
                    assembling = false;
                } else if (r != DeframerResult::IN_PROGRESS) {
                    assembling = false;
                }
            }
        }

        locked = lock;
    }

    demod.terminate();
    return lines;
}

const char *LINE = "K0RET-4>APFII0,TCPIP,K0RET-10*::K0RET-2  :Try 2{3F8E8";

} // namespace

TEST_CASE("M17 receive: m17-gateway's APRS packet after silence",
          "[m17][packet][aprs]")
{
    std::vector<int8_t> syms(2400, 0);
    appendGatewayPacket(syms, LINE);
    syms.insert(syms.end(), 400, 0);

    auto lines = receive(baseband(syms));
    REQUIRE(lines.size() == 1);
    CHECK(lines[0] == LINE);
}
