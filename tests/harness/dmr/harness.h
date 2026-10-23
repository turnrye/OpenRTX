/*
 * SPDX-FileCopyrightText: Copyright 2020-2026 OpenRTX Contributors
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

#ifndef DMR_HARNESS_H
#define DMR_HARNESS_H

#include <cstdint>
#include <vector>
#include <string>

/**
 * The DMR driver harness: the HR_C6000 DMR driver running on the host against
 * a model of the chip, a simulated clock and fakes of the interrupt lines, the
 * payload bus and the audio, so that a scenario can hand the driver the events
 * a transmission produces and check what it writes to the chip and reports.
 *
 * Time only passes when a scenario advances it, or when the driver sleeps.
 */
namespace harness
{

/** One write the driver made to the chip. */
struct Write {
    uint8_t page;   ///< Bus operating mode: 1 AUX, 2 DATA, 4 CONFIG...
    uint16_t addr;  ///< Register address.
    uint8_t value;  ///< Value written.
    long long tick; ///< Simulated time of the write, ms.
};

/** Reset every fake and the clock, before a scenario. */
void reset();

/** Append the scenario's writes and log to the DMR_HARNESS_TRACE file. */
void trace();

/** Start the driver with a colour code, as the rtx layer does. */
void start(uint8_t colorCode);

/** Start the driver on a repeater channel. */
void startRepeater(uint8_t colorCode, uint8_t timeslot);

/** Reconfigure the running driver, as the rtx layer does on a new channel. */
void configure(bool repeater, uint8_t timeslot, uint8_t colorCode);

/** Stop the driver. */
void stop();

/** Simulated time, ms. */
long long now();

/** Move the clock forward. */
void advance(long long ms);

/** Move the clock to a time, if that is later than now. */
void setTime(long long ms);

/** Wait until the driver thread has handled everything and is waiting. */
void waitIdle();

/** Raise the timeslot interrupt and wait for the driver to handle it. */
void timeslot();

/**
 * A slot boundary, the slot just ended carrying a burst with this status and
 * colour code. If the slot action the driver wrote for that slot silenced it
 * (0x41 = 0x00), the chip did not receive it and its status registers keep
 * what the slot before left there, as seen on the bench.
 */
void slot(uint8_t status, uint8_t colorCode);

/** Raise the timeslot interrupt several times before the driver gets to run,
 * as when its thread is woken late. */
void timeslot(int count);

/**
 * A slot boundary of a repeater's downlink: as slot(), with the CACH's TC bit
 * read alongside the colour code, and the sync source register telling a
 * repeater's sync (true) from a radio's.
 */
void downlinkSlot(uint8_t status, uint8_t colorCode, uint8_t tc,
                  bool repeater = true);

/** Raise the system interrupt and wait for the driver to handle it. */
void system();

/** What the chip reads back on a register, and interrupt flags it raises. */
void setReg(uint8_t page, uint16_t addr, uint8_t value);
uint8_t getReg(uint8_t page, uint16_t addr);
void raiseFlags(uint8_t flags);

/** Link control the chip hands over on the data page, 12 bytes. */
void setLinkControl(const std::vector<uint8_t> &lc);

/** Voice payload the chip hands over, 27 bytes. */
void setPayload(const std::vector<uint8_t> &payload);

/**
 * Voice payload of a downlink block, 27 bytes, 0 for the block at 0x30 and 1
 * for the one at 0x60, with the repeater's voice sync ahead of it or not.
 */
void setRepeaterPayload(uint8_t block, const std::vector<uint8_t> &payload,
                        bool voiceSync);

/** The writes the driver made, in order, and a filter by register. */
std::vector<Write> writes();
std::vector<Write> writes(uint8_t page, uint16_t addr);
void clearWrites();

/** Voice frames handed to playback, and the last one, 9 bytes. */
int audioFramesPushed();
std::vector<uint8_t> lastFramePushed();

/** Payloads written to the payload bus, 27 bytes each. */
std::vector<std::vector<uint8_t>> payloadsWritten();

/** The driver's log lines. */
std::vector<std::string> logLines();

/** The receive state as the rtx layer polls it. */
struct RxState {
    uint32_t srcId;
    uint32_t dstId;
    uint8_t colorCode;
    bool groupCall;
    bool encrypted;
    bool valid;
};
RxState poll();

/** Key up and unkey, as the rtx layer does. */
void keyUp(bool group, uint32_t dst, uint32_t src);
void unkey();
bool txActive();

/** The rtx layer's periodic transmit housekeeping call. */
void txTask();

/** A burst event: status and colour code as the chip reports them, then the
 * system interrupt with the given flags. */
void burst(uint8_t status, uint8_t colorCode, uint8_t flags = 0x08);

}

#endif
