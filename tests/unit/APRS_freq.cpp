/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#include <catch2/catch_test_macros.hpp>

#include "protocols/APRS/packet.h"
#include <cstring>
#include <string>

namespace
{

aprsPacket packetOf(const char *info, const char *dest = "APRS")
{
    // The parsed packet points into the frame: a few are kept, so that
    // packets parsed one after another stay valid together
    static uint8_t frames[4][APRS_PACLEN];
    static size_t nextFrame;
    uint8_t *frame = frames[nextFrame++ % 4];
    ssize_t n = aprsFrame_build(frame, APRS_PACLEN, dest, "K0RET-10", nullptr,
                                info, strlen(info));
    REQUIRE(n > 0);
    aprsPacket pkt;
    REQUIRE(aprsPkt_fromFrame(frame, n, &pkt) == 0);
    return pkt;
}

aprsFreq freqOf(const char *info, const char *dest = "APRS")
{
    aprsPacket pkt = packetOf(info, dest);
    aprsFreq f;
    INFO("info: " << info);
    REQUIRE(aprsFreqParse(&pkt, &f) == true);
    return f;
}

bool hasFreq(const char *info, const char *dest = "APRS")
{
    aprsPacket pkt = packetOf(info, dest);
    aprsFreq f;
    return aprsFreqParse(&pkt, &f);
}

} // namespace

TEST_CASE("APRS frequency: repeaters heard through the M17 gateway",
          "[aprs][freq]")
{
    /* The hotspot: 441.1125 sent to the spec's 1 kHz */
    aprsFreq f = freqOf("!3512.68NM08955.62W&441.112MHz M17 CAN0 M17-KCW B");
    CHECK(f.freq == 441112500);
    CHECK(f.mode == APRS_FREQ_M17);
    CHECK((f.flags & APRS_FREQ_HAS_CAN) != 0);
    CHECK(f.can == 0);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) == 0);
    CHECK(f.toneType == APRS_TONE_NONE);

    /* Frequency objects: named for it, tone and offset in the comment */
    f = freqOf(";146.625TN*111542z3505.07N/08948.50WrPHG6460 T107 -060 "
               "W4BS DARC");
    CHECK(f.freq == 146625000);
    CHECK(f.mode == APRS_FREQ_FM);
    CHECK(f.toneType == APRS_TONE_ENCODE);
    CHECK(f.tone == 107);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) != 0);
    CHECK(f.offset == -600000);
    CHECK((f.flags & APRS_FREQ_NARROW) == 0);

    f = freqOf(";224.780TN*111542z3505.07N/08948.50WrT107 -160");
    CHECK(f.freq == 224780000);
    CHECK(f.offset == -1600000);

    f = freqOf(";444.775TN*111542z3505.07N/08948.50WrT107 +500 Allstar");
    CHECK(f.freq == 444775000);
    CHECK(f.offset == 5000000);
    CHECK(f.mode == APRS_FREQ_FM);

    f = freqOf(";444.400TN*111542z3505.07N/08948.50WrT107 +500 Fusion");
    CHECK(f.mode == APRS_FREQ_FUSION);
    CHECK(f.tone == 107);

    /* No offset: the band's standard applies */
    f = freqOf(";146.82TN *111111z3509.15N/09002.69WrPHG7560 T107 8:00pm NET "
               "W4BS DARC");
    CHECK(f.freq == 146820000);
    CHECK(f.tone == 107);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) == 0);

    /* "+" is the name's unique ID, not a shift */
    f = freqOf(";444.175+ *111111z3508.70N/09001.88Wr W4EM DMR Repeater CC4");
    CHECK(f.freq == 444175000);
    CHECK(f.mode == APRS_FREQ_DMR);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) == 0);

    /* A Winlink gateway object, the frequency opening its comment */
    f = freqOf(";K4SPB-10 *110319z3508.  NW08947.  Wa144.990MHz Winlink VARA "
               "FM Wide Gateway");
    CHECK(f.freq == 144990000);
    CHECK(f.mode == APRS_FREQ_DATA);
}

TEST_CASE("APRS frequency: repeaters on APRS-IS", "[aprs][freq]")
{
    /* From tests/unit/data/aprs_is_sea150.txt */
    aprsFreq f = freqOf(";KC7CKO   *111111z4650.58N/12245.86WrPHG5350/"
                        "146.800MHz t110 -060 R26k Crawford VHF,UHF,220 "
                        "repeaters");
    CHECK(f.freq == 146800000);
    CHECK(f.toneType == APRS_TONE_ENCODE);
    CHECK(f.tone == 110);
    CHECK((f.flags & APRS_FREQ_NARROW) != 0);
    CHECK(f.offset == -600000);
    CHECK(f.mode == APRS_FREQ_FM);

    f = freqOf(";LFPARK   *111111z4746.31N/12216.85WrPHG5360/224.220MHz "
               "T123 R28k WA7FUS - Lake Forest (pnwsvx.net)");
    CHECK(f.freq == 224220000);
    CHECK(f.tone == 123);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) == 0);

    /* A D-STAR gateway says only "DV"; "+5 MHz" is not the spec's */
    f = freqOf(";WA7FW  B *110505z4718.91ND12220.08WaRNG0040 443.850MHz,+5 "
               "MHz, DV");
    CHECK(f.freq == 443850000);
    CHECK(f.mode == APRS_FREQ_DSTAR);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) == 0);

    f = freqOf("=4727.94N/12140.77W$444.550MHz/A=000753 aprsdroid");
    CHECK(f.freq == 444550000);

    /* Above 999.999 MHz the spec uses letters; not for this radio */
    CHECK(hasFreq("!4718.91ND12220.08W&RNG0040 1290.10MHz,-20 Mhz, DV")
          == false);
}

TEST_CASE("APRS frequency: the spec's comment fields", "[aprs][freq]")
{
    aprsFreq f = freqOf("!3500.00N/09000.00W-146.835MHz C107 R25m AARC");
    CHECK(f.freq == 146835000);
    CHECK(f.toneType == APRS_TONE_SQUELCH);
    CHECK(f.tone == 107);

    f = freqOf("!3500.00N/09000.00W-146.805MHz D256 R25k Repeater");
    CHECK(f.toneType == APRS_TONE_DCS);
    CHECK(f.tone == 256);

    /* "FFF.FF MHz" to 10 kHz */
    f = freqOf("!3500.00N/09000.00W-146.40 MHz T067 +100 Repeater");
    CHECK(f.freq == 146400000);
    CHECK(f.tone == 67);
    CHECK(f.offset == 1000000);

    f = freqOf("!3500.00N/09000.00W-442.440MHz T107 -500 Repeater");
    CHECK(f.offset == -5000000);

    /* Lower case is narrow FM */
    f = freqOf("!3500.00N/09000.00W-145.50 MHz t077 Simplex");
    CHECK(f.toneType == APRS_TONE_ENCODE);
    CHECK((f.flags & APRS_FREQ_NARROW) != 0);

    f = freqOf("!3500.00N/09000.00W-439.250MHz l750 -760");
    CHECK(f.toneType == APRS_TONE_BURST);
    CHECK(f.tone == 1750);
    CHECK((f.flags & APRS_FREQ_NARROW) != 0);
    CHECK(f.offset == -7600000);

    f = freqOf("!3500.00N/09000.00W-145.600MHz 1750 -060");
    CHECK(f.toneType == APRS_TONE_BURST);
    CHECK((f.flags & APRS_FREQ_NARROW) == 0);

    /* "Toff": no tone, forced simplex; so is "-000" */
    f = freqOf("!3500.00N/09000.00W-147.120MHz Toff");
    CHECK(f.toneType == APRS_TONE_NONE);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) != 0);
    CHECK(f.offset == 0);

    f = freqOf("!3500.00N/09000.00W-147.120MHz T100 -000");
    CHECK(f.tone == 100);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) != 0);
    CHECK(f.offset == 0);

    /* Case-insensitive on receipt; a delimiter may lead */
    f = freqOf("!3500.00N/09000.00W-/146.520mhz");
    CHECK(f.freq == 146520000);
    f = freqOf("!3500.00N/09000.00W- 146.520MHZ");
    CHECK(f.freq == 146520000);

    /* After an altitude */
    f = freqOf("!3500.00N/09000.00W-/A=000123 146.520MHz");
    CHECK(f.freq == 146520000);

    /* After the course/speed extension */
    f = freqOf("!3500.00N/09000.00W>090/036/146.520MHz T100");
    CHECK(f.freq == 146520000);
    CHECK(f.tone == 100);
}

TEST_CASE("APRS frequency: split and crossband", "[aprs][freq]")
{
    /* "FFF.FFFrx": where the repeater listens */
    aprsFreq f = freqOf("!3500.00N/09000.00W-146.940MHz T100 147.540rx");
    CHECK(f.freq == 146940000);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) != 0);
    CHECK(f.offset == 600000);

    /* Named for its output, a comment frequency is its input */
    f = freqOf(";146.94-AB*111111z3500.00N/09000.00Wr447.940MHz T100");
    CHECK(f.freq == 146940000);
    CHECK(f.offset == 301000000);
    CHECK(f.tone == 100);

    /* The same frequency twice, for radios that skip the name */
    f = freqOf(";146.82TN *111111z3500.00N/09000.00Wr146.820MHz T107");
    CHECK(f.freq == 146820000);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) == 0);
    CHECK(f.tone == 107);
}

TEST_CASE("APRS frequency: modes", "[aprs][freq]")
{
    aprsFreq f = freqOf("!3500.00N/09000.00W-145.670MHz D-STAR>KK4ABC B");
    CHECK(f.mode == APRS_FREQ_DSTAR);

    /* A word for FM marks a mixed repeater */
    f = freqOf("!3500.00N/09000.00Wr442.100MHz T100 DMR/FM");
    CHECK(f.mode == APRS_FREQ_FM);
    CHECK(f.tone == 100);

    f = freqOf("!3500.00N/09000.00Wr442.100MHz P25 NAC293");
    CHECK(f.mode == APRS_FREQ_P25);
    f = freqOf("!3500.00N/09000.00Wr442.100MHz NXDN");
    CHECK(f.mode == APRS_FREQ_NXDN);
    f = freqOf("!3500.00N/09000.00Wr442.100MHz C4FM");
    CHECK(f.mode == APRS_FREQ_FUSION);
    f = freqOf("!3500.00N/09000.00Wr145.010MHz Packet BBS");
    CHECK(f.mode == APRS_FREQ_DATA);

    /* The channel access number, in the forms people write it */
    f = freqOf("!3500.00N/09000.00Wr441.112MHz M17 CAN 3");
    CHECK(f.can == 3);
    f = freqOf("!3500.00N/09000.00Wr441.112MHz M17 can=7");
    CHECK(f.can == 7);
    f = freqOf("!3500.00N/09000.00Wr441.112MHz M17 CAN16");
    CHECK((f.flags & APRS_FREQ_HAS_CAN) == 0);
    f = freqOf("!3500.00N/09000.00Wr441.112MHz M17");
    CHECK(f.mode == APRS_FREQ_M17);
    CHECK((f.flags & APRS_FREQ_HAS_CAN) == 0);
}

TEST_CASE("APRS frequency: other packets that carry one", "[aprs][freq]")
{
    /* Trackers that cannot put it in the position put it in the status */
    aprsFreq f = freqOf(">146.520MHz T100 Monitoring");
    CHECK(f.freq == 146520000);
    CHECK(f.tone == 100);
    f = freqOf(">111111z147.120MHz");
    CHECK(f.freq == 147120000);

    /* Kenwood's Mic-E: radio type, altitude, then the frequency */
    f = freqOf("`(_fn\"Oj/]\"4T}146.520MHz T100=", "S32U6T");
    CHECK(f.freq == 146520000);
    CHECK(f.tone == 100);
    f = freqOf("`(_fn\"Oj/>146.820MHz T107 -060^", "S32U6T");
    CHECK(f.freq == 146820000);
    CHECK(f.offset == -600000);
}

TEST_CASE("APRS frequency: what is not one", "[aprs][freq]")
{
    /* No frequency, or not where the spec puts it */
    CHECK(hasFreq("!3500.00N/09000.00W-Just a comment") == false);
    CHECK(hasFreq("!3500.00N/09000.00W-Call me on 146.520MHz") == false);
    CHECK(hasFreq("!3500.00N/09000.00W-146.520") == false);
    CHECK(hasFreq("!3500.00N/09000.00W-146.5MHz") == false);
    CHECK(hasFreq(">status text") == false);
    CHECK(hasFreq(":K1ABC    :146.520MHz") == false);
    CHECK(hasFreq(";EL-123456*111111z3500.00N/09000.00WE") == false);

    /* Free text ends the fields: a tone in it is not taken */
    aprsFreq f = freqOf("!3500.00N/09000.00W-146.520MHz Net Tue T107 +060");
    CHECK(f.toneType == APRS_TONE_NONE);
    CHECK((f.flags & APRS_FREQ_HAS_OFFSET) == 0);

    /* 1 kHz is only filled out for 12.5 kHz channels */
    CHECK(freqOf("!3500.00N/09000.00W-446.137MHz").freq == 446137500);
    CHECK(freqOf("!3500.00N/09000.00W-147.105MHz").freq == 147105000);
    CHECK(freqOf("!3500.00N/09000.00W-441.122MHz").freq == 441122000);
    CHECK(freqOf("!3500.00N/09000.00W-441.1125MHz").freq == 441112500);
}
